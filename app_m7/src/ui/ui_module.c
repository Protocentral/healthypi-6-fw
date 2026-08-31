/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * UI engine. Owns the LVGL service thread, the screen manager + nav, and
 * the sample-bus subscription. The UI is the ONLY caller of LVGL here: the bus
 * is drained in this thread (the producer just deposits into the consumer ring),
 * so the UI never runs on a producer's callback — the deferred-update rule.
 */
#include "ui_module.h"

#include <zephyr/kernel.h>

#if IS_ENABLED(CONFIG_HPI_DISPLAY_ENABLED)

#include <lvgl.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led.h>

#include "theme/hpi_m3_theme.h"
#include "components/hpi_ui_components.h"
#include "screens/scr_home.h"
#include "screens/scr_live.h"
#include "screens/scr_secondary.h"
#include "screens/scr_more.h"
#include "screens/scr_boot.h"
#include "screens/scr_trends.h"
#include "screens/scr_link.h"
#include "screens/scr_settings.h"
#include "screens/scr_ambient.h"
#include "screens/scr_power.h"
#include "core/sample_bus.h"
#include "core/sample_formats.h"
#include "core/channel_registry.h"
#include "services/recording_service.h"
#include "services/config_service.h"
#include "services/button_service.h"

LOG_MODULE_REGISTER(hpi_ui, CONFIG_HPI_APP_LOG_LEVEL);

/* --- UI bring-up tracing ---------------------------------------------------
 * The screens are built back to back out of one LVGL heap, and heap exhaustion
 * does not fail loudly: lv_malloc returns NULL and the NULL is carried until a
 * later dereference lands as an *imprecise* bus fault far from the cause. Log
 * each step with the heap high-water so the last line before a fault names the
 * screen that ran out. Costs nothing when CONFIG_SYS_HEAP_RUNTIME_STATS is off. */
#if IS_ENABLED(CONFIG_SYS_HEAP_RUNTIME_STATS)
#include <lvgl_mem.h>
#endif

static void ui_trace(const char *step)
{
#if IS_ENABLED(CONFIG_SYS_HEAP_RUNTIME_STATS)
	struct sys_memory_stats st = { 0 };

	lvgl_heap_stats(&st);
	LOG_INF("build: %-10s lvgl heap used=%zu peak=%zu free=%zu",
		step, st.allocated_bytes, st.max_allocated_bytes, st.free_bytes);
#else
	LOG_INF("build: %s", step);
#endif
}

/* The UI is a decimating live-preview consumer drained every loop (~30 ms), so
 * a small ring is plenty. It also subscribes LAST, so it must stay frugal with
 * the shared bus heap: a larger ring (32 frames ~= 21 KB) overflows the 64 KB
 * heap and the UI gets no data at all. 8 frames buffers ~2 drain cycles. */
#define UI_RING_FRAMES   8
#define UI_LOOP_CAP_MS   30
/* Service-backed screens (Record/Status) poll their service this often; the
 * bus-backed Home screen updates per-frame in ui_drain_bus() instead. */
#define UI_REFRESH_MS    500
/* Display decimation: bring both signals down to a ~125 Hz sweep rate so the
 * chart point count maps to a fixed time window (ECG ~490 S/s -> /4; PPG
 * ~252 S/s -> /2). Keep in sync with HPI_HOME_WAVE_POINTS and the real
 * acquisition rates (see the `acq:` debug line) if either changes. */
#define ECG_DECIM        4
#define PPG_DECIM        2

static lv_obj_t *s_content;
static lv_obj_t *s_navbar;
/* The boot overlay, kept so it can be lifted back above the screens that are
 * built after it. Set to NULL when the overlay is destroyed. */
static lv_obj_t *s_boot_overlay;
static lv_obj_t *s_screen[HPI_UI_SCREEN_COUNT];
static enum hpi_ui_screen s_active = HPI_UI_SCREEN_HOME;
static struct hpi_bus_sub *s_sub;
static bool s_nav_locked;   /* nav hidden + swipe blocked (record-active) */
/* Ambient entered by the button rather than by idling. Needed because a button
 * press does not touch LVGL, so the idle test below would otherwise wake the
 * display on the very next loop. Cleared only by real input activity. */
static bool s_manual_sleep;
static uint32_t s_prev_idle;

/* Boot sequence (splash -> self-test -> main UI). The self-test holds until every
 * subsystem is up (min dwell so it's visible), then advances; a max timeout is a
 * fallback so a genuinely-failed subsystem can't brick the boot (M4 binds ~7 s). */
#define BOOT_SPLASH_MS        1500
#define BOOT_SELFTEST_MIN_MS  1500
#define BOOT_SELFTEST_MAX_MS  8000
static enum { BOOT_SPLASH, BOOT_SELFTEST, BOOT_DONE } s_boot = BOOT_SPLASH;
static int64_t s_boot_t0;

/* Ambient-clock sleep: dim to AMBIENT_BRIGHT after the configured idle period.
 * The timeout is Settings -> Display sleep (hpi_config_display_sleep_s), not a
 * constant. 0 seconds means never sleep. */
#define AMBIENT_BRIGHT      5

#if DT_NODE_EXISTS(DT_NODELABEL(backlight))
static const struct device *s_backlight;
#endif

void hpi_ui_set_brightness(uint8_t pct)
{
#if DT_NODE_EXISTS(DT_NODELABEL(backlight))
	if (pct > 100) {
		pct = 100;
	}
	if (s_backlight && device_is_ready(s_backlight)) {
		led_set_brightness(s_backlight, 0, pct);
	}
#else
	ARG_UNUSED(pct);
#endif
}

void hpi_ui_request_sleep(void)
{
	if (!hpi_scr_ambient_active()) {
		hpi_scr_ambient_show(lv_screen_active());
	}
	hpi_ui_set_brightness(AMBIENT_BRIGHT);
	s_manual_sleep = true;
}

void hpi_ui_show_screen(enum hpi_ui_screen scr)
{
	if (scr >= HPI_UI_SCREEN_COUNT) {
		return;
	}
	s_active = scr;
	for (int i = 0; i < HPI_UI_SCREEN_COUNT; i++) {
		if (s_screen[i] == NULL) {
			continue;
		}
		if (i == (int)scr) {
			lv_obj_clear_flag(s_screen[i], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(s_screen[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (s_navbar) {
		hpi_ui_navbar_set_active(s_navbar, scr);   /* no delete — safe in click cb */
	}
}

/* Left/right swipe cycles the main waveform/data tabs (Home..Trends). "More"
 * and its submenu are tap-only, so swipe is ignored there. */
static void ui_gesture_cb(lv_event_t *e)
{
	ARG_UNUSED(e);
	lv_indev_t *indev = lv_indev_active();

	if (indev == NULL || s_nav_locked || s_active > HPI_UI_LAST_SWIPE_TAB) {
		return;
	}

	switch (lv_indev_get_gesture_dir(indev)) {
	case LV_DIR_LEFT:
		if (s_active < HPI_UI_LAST_SWIPE_TAB) {
			hpi_ui_show_screen((enum hpi_ui_screen)(s_active + 1));
		}
		break;
	case LV_DIR_RIGHT:
		if (s_active > HPI_UI_SCREEN_HOME) {
			hpi_ui_show_screen((enum hpi_ui_screen)(s_active - 1));
		}
		break;
	default:
		break;
	}
}

/* Phase 1 of bring-up: the screen shell, the splash, and the backlight.
 * Split from the screen build so the panel lights up as early as possible:
 * the splash depends on nothing else, so it is built and flushed first, and
 * the rest is built behind it.
 */
static void ui_build_splash(void)
{
	lv_obj_t *scr = lv_screen_active();

	hpi_m3_apply_screen(scr);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_pad_row(scr, 0, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_event_cb(scr, ui_gesture_cb, LV_EVENT_GESTURE, NULL);

	s_content = lv_obj_create(scr);
	lv_obj_set_width(s_content, lv_pct(100));
	lv_obj_set_flex_grow(s_content, 1);
	lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s_content, 0, 0);
	lv_obj_set_style_pad_all(s_content, 0, 0);
	lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

	s_boot_overlay = hpi_scr_boot_create(scr);
	ui_trace("boot-ovl");
	s_boot = BOOT_SPLASH;
	s_boot_t0 = k_uptime_get();

#if DT_NODE_EXISTS(DT_NODELABEL(backlight))
	const struct device *bl = DEVICE_DT_GET(DT_NODELABEL(backlight));

	s_backlight = bl;   /* remember for hpi_ui_set_brightness (Settings/ambient) */

	/* TPS61158 backlight via TIM3_CH1 PWM (PC6). Log every failure path so a
	 * blank screen can be told apart from a panel/LTDC flush problem: if this
	 * logs OK but the screen is still dark, the backlight is not the culprit. */
	if (!device_is_ready(bl)) {
		LOG_ERR("backlight device not ready (PWM/LED-PWM init failed)");
	} else {
		int ret = led_set_brightness(bl, 0, 100);

		if (ret < 0) {
			LOG_ERR("backlight led_set_brightness failed (%d)", ret);
		} else {
			LOG_INF("backlight on (TIM3_CH1 PWM, 100%%)");
		}
	}
#else
	LOG_WRN("no 'backlight' DT node - backlight will stay off");
#endif
}

/* Phase 2: everything the splash is covering. */
static void ui_build_screens(void)
{
	lv_obj_t *scr = lv_screen_active();

	/* Main tabs. */
	ui_trace("shell");
	s_screen[HPI_UI_SCREEN_HOME]   = hpi_scr_home_create(s_content);
	ui_trace("home");
	s_screen[HPI_UI_SCREEN_LIVE]   = hpi_scr_live_create(s_content);
	ui_trace("live");
	s_screen[HPI_UI_SCREEN_REC]    = hpi_scr_record_create(s_content);
	ui_trace("rec");
	s_screen[HPI_UI_SCREEN_TRENDS] = hpi_scr_trends_create(s_content);
	ui_trace("trends");
	s_screen[HPI_UI_SCREEN_MORE]   = hpi_scr_more_create(s_content);
	ui_trace("more");
	/* More submenu. */
	s_screen[HPI_UI_SCREEN_LINK]     = hpi_scr_link_create(s_content);
	ui_trace("link");
	s_screen[HPI_UI_SCREEN_HRV]      = hpi_scr_hrv_create(s_content);
	ui_trace("hrv");
	s_screen[HPI_UI_SCREEN_SETTINGS] = hpi_scr_settings_create(s_content);
	ui_trace("settings");
	s_screen[HPI_UI_SCREEN_ALERT]    = hpi_scr_alert_create(s_content);
	ui_trace("alert");
	s_screen[HPI_UI_SCREEN_OTA]      = hpi_scr_ota_create(s_content);
	ui_trace("ota");

	s_navbar = hpi_ui_navbar_create(scr, HPI_UI_SCREEN_HOME);
	ui_trace("navbar");
	hpi_ui_show_screen(HPI_UI_SCREEN_HOME);

	/* Populate every screen's status bar once here, so the first screen shown
	 * after the splash already has a clock and a battery reading rather than
	 * the "--" placeholders they are built with. */
	hpi_ui_statusbar_refresh();

	/* Nav stays hidden until the boot overlay tears down. */
	lv_obj_add_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);

	/* The overlay was created before any of these, so it is now BELOW them in
	 * the child order -- LVGL draws siblings in creation order. Lift it back to
	 * the top or the screens just built would cover the splash that is meant to
	 * be hiding them. */
	if (s_boot_overlay != NULL) {
		lv_obj_move_foreground(s_boot_overlay);
	}
}

/* Hard cap on frames consumed per drain. An uncapped `while (pull() == 0)`
 * against a live producer is a live-lock: if per-frame work ever costs more
 * than the producer takes to refill, the loop never empties and the UI thread
 * never reaches lv_timer_handler() again. Dropping frames is correct for a
 * preview consumer, and the bus already counts the drops. */
#define UI_DRAIN_MAX  (UI_RING_FRAMES * 2)

static void ui_drain_bus(void)
{
	static uint32_t ne, np;
	struct hpi_sample_frame f;
	bool home = (s_active == HPI_UI_SCREEN_HOME);
	bool live = (s_active == HPI_UI_SCREEN_LIVE);
	unsigned int budget = UI_DRAIN_MAX;

	while (budget-- > 0 && hpi_bus_pull(s_sub, &f) == 0) {
		switch (f.channel) {
		case HPI_CH_ECG: {
			const struct hp6_ecg_sample *s = f.payload;
			for (uint16_t i = 0; i < f.sample_count; i++) {
				if ((home || live) && (ne++ % ECG_DECIM) == 0) {
					/* lead_off travels with the samples, so both
					 * screens learn the electrode state directly
					 * from the producer -- it does not depend on
					 * the M4 being alive to send vitals. */
					if (home) {
						hpi_scr_home_push_ecg(s[i].lead_ii,
								      s[i].lead_off);
					} else {  /* Live: ECG lane + Resp lane (resp rides in the ECG sample) */
						hpi_scr_live_push_ecg(s[i].lead_ii, s[i].resp,
								      s[i].lead_off);
					}
				}
			}
			break;
		}
		case HPI_CH_PPG: {
			const struct hp6_ppg_sample *s = f.payload;
			for (uint16_t i = 0; i < f.sample_count; i++) {
				if ((home || live) && (np++ % PPG_DECIM) == 0) {
					if (home) {
						hpi_scr_home_push_ppg(s[i].ir);
					} else {
						hpi_scr_live_push_ppg(s[i].ir);
					}
				}
			}
			break;
		}
		case HPI_CH_VITALS: {
			const struct hp6_vitals *v = f.payload;
			hpi_scr_home_set_vitals(v);
			hpi_scr_live_set_vitals(v);
			hpi_scr_trends_push_vitals(v);   /* accumulate history */
			hpi_scr_ambient_set_hr(v->hr_bpm, v->flags);
			break;
		}
		default:
			break;
		}
	}
}

static void ui_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	/* Runtime self-stub: if the panel is absent/broken the display driver
	 * fails init and LVGL's Zephyr glue has no usable display —
	 * building/flushing the UI anyway corrupts memory and bus-faults. Degrade
	 * to headless instead: acquisition, USB, recording and MCUmgr keep
	 * working, matching CONFIG_HPI_DISPLAY_ENABLED=n. */
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

	if (!device_is_ready(disp)) {
		LOG_ERR("display not ready (LTDC init failed) — "
			"UI disabled, running headless");
		return;
	}

#if DT_NODE_EXISTS(DT_NODELABEL(gc9503v))
	/* chosen zephyr,display is the LTDC, which inits fine without a panel —
	 * the DSI panel behind it is what fails when disconnected. Check it too. */
	const struct device *panel = DEVICE_DT_GET(DT_NODELABEL(gc9503v));

	if (!device_is_ready(panel)) {
		LOG_ERR("display panel not ready (absent / DSI init failed) — "
			"UI disabled, running headless");
		return;
	}
#endif

	/* Splash first, then one lv_timer_handler() to actually put it on the
	 * panel, then everything else — without the flush in between the split
	 * buys nothing. */
	ui_build_splash();
	(void)lv_timer_handler();
	ui_trace("splash-up");

	ui_build_screens();
	ui_trace("built");

	struct hpi_bus_sub_cfg cfg = {
		.name = "ui",
		.channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
				HPI_CH_BIT(HPI_CH_VITALS),
		.ring_frames = UI_RING_FRAMES,
	};
	s_sub = hpi_bus_subscribe(&cfg);
	if (s_sub == NULL) {
		LOG_ERR("UI bus subscribe failed");
	}
	LOG_INF("UI up (Material 3, %d screens)", HPI_UI_SCREEN_COUNT);

	int64_t next_refresh = 0;
#if IS_ENABLED(CONFIG_HPI_UI_DEBUG_CYCLE)
	int64_t s_cycle_at = 0;
#endif

	while (1) {
		/* Boot overlay: splash -> self-test -> tear down, then reveal nav. */
		if (s_boot != BOOT_DONE) {
			int64_t el = k_uptime_get() - s_boot_t0;

			if (s_boot == BOOT_SPLASH && el >= BOOT_SPLASH_MS) {
				hpi_scr_boot_selftest();
				s_boot = BOOT_SELFTEST;
				s_boot_t0 = k_uptime_get();
			} else if (s_boot == BOOT_SELFTEST) {
				hpi_scr_boot_refresh();
				bool pass = hpi_scr_boot_all_pass();

				/* Advance once subsystems are up (after a min dwell), or
				 * on the max-timeout fallback if one never comes up. */
				if ((pass && el >= BOOT_SELFTEST_MIN_MS) ||
				    el >= BOOT_SELFTEST_MAX_MS) {
					hpi_scr_boot_destroy();
					/* The overlay object is gone; drop our handle so a
					 * later move_foreground cannot touch freed memory. */
					s_boot_overlay = NULL;
					if (s_navbar) {
						lv_obj_clear_flag(s_navbar, LV_OBJ_FLAG_HIDDEN);
					}
					s_boot = BOOT_DONE;
				}
			}
		}

		/* Hardware button. The service decodes the gesture and handles the
		 * mark itself; what reaches here is the presentation half of the
		 * button map, polled so the dependency stays L5 -> L4. */
		if (s_boot == BOOT_DONE) {
			enum hpi_button_action act = hpi_button_take_action();

			/* A button press never reaches LVGL, so
			 * lv_display_get_inactive_time() keeps climbing across the
			 * gesture. Reset it for the gestures that mean "stay awake",
			 * or the idle block below drops straight back to the ambient
			 * clock. Not for TOGGLE_SLEEP: that one means the opposite,
			 * and s_manual_sleep holds it there. */
			if (act == HPI_BTN_HOME || act == HPI_BTN_POWER_MENU) {
				lv_display_trigger_activity(NULL);
			}

			switch (act) {
			case HPI_BTN_TOGGLE_SLEEP:
				if (hpi_scr_ambient_active()) {
					hpi_scr_ambient_hide();
					hpi_ui_set_brightness(hpi_config_display_brightness());
					s_manual_sleep = false;
				} else {
					hpi_ui_request_sleep();
				}
				break;
			case HPI_BTN_HOME:
				if (hpi_scr_ambient_active()) {
					hpi_scr_ambient_hide();
					hpi_ui_set_brightness(hpi_config_display_brightness());
				}
				s_manual_sleep = false;
				hpi_ui_show_screen(HPI_UI_SCREEN_HOME);
				break;
			case HPI_BTN_POWER_MENU:
				/* No "power off" here: the board is switched by a
				 * slide switch and the SoC has no poweroff to call. */
				if (hpi_scr_ambient_active()) {
					hpi_scr_ambient_hide();
					s_manual_sleep = false;
				}
				/* Unconditionally, not just when waking: the menu has
				 * to be readable however the display got here. */
				hpi_ui_set_brightness(hpi_config_display_brightness());
				hpi_scr_power_show(lv_screen_active());
				break;
			case HPI_BTN_NONE:
			default:
				break;
			}
		}

		/* Ambient-clock sleep: dim + overlay after idle; any touch wakes.
		 * Suspended while the power menu is up -- a modal the user is
		 * reading must not dim itself out from under them. */
		if (s_boot == BOOT_DONE && !hpi_scr_power_active()) {
			uint32_t idle = lv_display_get_inactive_time(NULL);
			uint32_t sleep_ms = hpi_config_display_sleep_s() * 1000U;

			/* LVGL resets the inactive timer on any input, so a DROP since
			 * the last poll is the one reliable sign of real activity --
			 * and the only thing that ends a button-initiated sleep. */
			if (idle < s_prev_idle) {
				s_manual_sleep = false;
			}
			s_prev_idle = idle;

			if (hpi_scr_ambient_active()) {
				/* Wake on activity, or if sleep was switched off while
				 * the ambient clock was already up. */
				if (!s_manual_sleep && (sleep_ms == 0U || idle < sleep_ms)) {
					hpi_scr_ambient_hide();
					hpi_ui_set_brightness(hpi_config_display_brightness());
				} else {
					hpi_scr_ambient_refresh();
				}
			} else if (sleep_ms != 0U && idle >= sleep_ms) {
				hpi_scr_ambient_show(lv_screen_active());
				hpi_ui_set_brightness(AMBIENT_BRIGHT);
			}
		}

		if (s_sub) {
			ui_drain_bus();
		}

		/* Record/Status read their services, not the bus -- poll them on a
		 * slow cadence (this is the UI thread, so the calls are LVGL-safe). */
		int64_t now = k_uptime_get();

		if (now >= next_refresh) {
			next_refresh = now + UI_REFRESH_MS;

			/* Hide nav + block swipe while actively recording on the
			 * Record screen; restored on stop / leaving. */
			bool lock = (s_active == HPI_UI_SCREEN_REC) &&
				    hpi_recording_active();
			if (lock != s_nav_locked) {
				s_nav_locked = lock;
				if (s_navbar) {
					lv_obj_set_flag(s_navbar, LV_OBJ_FLAG_HIDDEN, lock);
				}
			}

#if IS_ENABLED(CONFIG_SYS_HEAP_RUNTIME_STATS)
			/* Rendering allocates too (draw tasks, layers, glyph
			 * cache), so the build-time figure is a floor, not the
			 * peak — 99 KB vs 176 KB on v5. Only in the measuring
			 * build (disp_debug.conf); this would otherwise log
			 * twice a second forever. */
			ui_trace("run");
#endif

#if IS_ENABLED(CONFIG_HPI_UI_DEBUG_CYCLE)
			/* Walk every screen so the high-water covers the whole set,
			 * not just whichever one boot happened to leave up. */
			if (s_boot == BOOT_DONE &&
			    now >= s_cycle_at) {
				s_cycle_at = now + CONFIG_HPI_UI_DEBUG_CYCLE_MS;
				hpi_ui_show_screen((enum hpi_ui_screen)
					((s_active + 1) % HPI_UI_SCREEN_COUNT));
				LOG_INF("debug-cycle -> screen %d", (int)s_active);
			}
#endif

			/* Refresh every screen's status bar unconditionally, so a
			 * tab change never reveals a stale clock. */
			hpi_ui_statusbar_refresh();

			if (s_active == HPI_UI_SCREEN_TRENDS) {
				hpi_scr_trends_refresh();
			} else if (s_active == HPI_UI_SCREEN_REC) {
				hpi_scr_record_refresh();
			} else if (s_active == HPI_UI_SCREEN_LINK) {
				hpi_scr_link_refresh();   /* Wi-Fi status */
			}
		}

		uint32_t next = lv_timer_handler();

		if (next == LV_NO_TIMER_READY || next > UI_LOOP_CAP_MS) {
			next = UI_LOOP_CAP_MS;
		}
		k_msleep(next);
	}
}

K_THREAD_STACK_DEFINE(ui_stack, 8192);
static struct k_thread ui_tcb;

void hpi_ui_init(void)
{
	/* Priority 8: below acquisition/IPC, a cooperative-friendly UI thread. */
	k_thread_create(&ui_tcb, ui_stack, K_THREAD_STACK_SIZEOF(ui_stack),
			ui_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&ui_tcb, "hpi_ui");
}

#else /* !CONFIG_HPI_DISPLAY_ENABLED -- headless build */

void hpi_ui_init(void) { }
void hpi_ui_show_screen(enum hpi_ui_screen scr) { ARG_UNUSED(scr); }
void hpi_ui_request_sleep(void) { }
void hpi_ui_set_brightness(uint8_t pct) { ARG_UNUSED(pct); }

#endif
