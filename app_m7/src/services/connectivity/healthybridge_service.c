/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Connectivity service -- see healthybridge_service.h. A bus consumer that maps
 * canonical hp6_* frames onto the HealthyBridge frame structs and forwards them
 * to the ESP32-C6 for WiFi/BLE re-streaming.
 *
 * Reaches the link through the vtable in healthybridge_esp32_link.h, so nothing
 * below depends on the physical transport (UART4 with hardware RTS/CTS today).
 */

#include "healthybridge_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <stddef.h>
#include <string.h>

#include <healthybridge_esp32_link.h>

#if IS_ENABLED(CONFIG_HPI_CONNECTIVITY) && defined(HEALTHYBRIDGE_LINK_NODE)
#define HB_AVAILABLE 1
#define HB_NODE HEALTHYBRIDGE_LINK_NODE

#include "core/sample_bus.h"
#include "core/sample_formats.h"
#include "core/channel_registry.h"

LOG_MODULE_REGISTER(hpi_conn, CONFIG_HPI_APP_LOG_LEVEL);

#define CONN_BATCH_MAX 32   /* bus frames batch <=16; cap defensively */

static const struct device *const hb = DEVICE_DT_GET(HB_NODE);
static struct hpi_bus_sub *g_sub;
static volatile bool g_started;     /* consumer thread running */
static uint32_t g_drops;            /* failed sends (ESP32 not ready/absent) */
static uint32_t g_ecg_n, g_ppg_n, g_vit_n;  /* frames forwarded per window */
static uint32_t g_skipped;          /* frames discarded because the link is off */

/*
 * Radio/link state. Mutated ONLY by conn_thread. Other threads -- the LVGL
 * thread, SMP handlers, the shell -- post an intent through g_req and return
 * immediately; none of them may block (powering the co-processor blocks for
 * most of a second). Nothing here is persisted: every boot starts with
 * g_req_radios = 0 and the link OFF -- a remembered "on" would reintroduce the
 * boot-time current spike (brownout on USB power).
 */
static volatile uint8_t g_req_radios;      /* requested HPI_CONN_RADIO_* mask */
static volatile bool    g_req_pending;     /* a change is waiting for the thread */
static volatile bool    g_req_powered;     /* co-processor should be powered */
static volatile bool    g_req_softap;      /* open the provisioning portal */

static uint8_t g_link_state = HPI_CONN_LINK_OFF;
static uint8_t g_radios;                   /* radios the thread has applied */

/* Last status the co-processor reported, and when. */
static struct hpi_hb_wifi_status_resp g_st;
static int64_t g_st_at;
static bool    g_st_valid;

/* A powered link that has not answered within this long is FAULT rather than
 * STARTING -- long enough to cover the boot wait plus a slow first reply. */
#define CONN_LINK_ANSWER_MS 5000

/*
 * Co-processor firmware version (HB GET_VERSION, 0x31), fetched by conn_thread
 * once the link is up and served from here -- an SMP handler or the LVGL thread
 * must never wait on the UART for it.
 *
 * Invalidated whenever the C6 can have changed underneath: power-down, a link
 * FAULT, and any restart the driver sees (release from reset, or an in-place
 * reboot such as the one after an OTA, detected by the C6's frame sequence
 * number running backwards -- see healthybridge_rx_stats.peer_restarts).
 *
 * An image older than HealthyBridge 1.1.0 acks the command with an error; that
 * is final for this boot of the C6. A peer that does not answer (busy, or an
 * HP5-profile image) is retried a few times, one poll window apart, then left
 * alone until it next restarts.
 */
enum conn_ver_state {
	CONN_VER_UNKNOWN = 0,   /* not asked yet, or asked and no answer */
	CONN_VER_KNOWN,         /* g_ver holds the C6's version string */
	CONN_VER_UNSUPPORTED,   /* the C6 predates GET_VERSION */
};
#define CONN_VER_TRIES 3

static struct k_spinlock g_ver_lock;           /* g_ver + g_ver_state */
static char g_ver[SIZEOF_FIELD(struct hpi_hb_version_resp, version_string)];
static uint8_t g_ver_state = CONN_VER_UNKNOWN;
static uint8_t g_ver_tries;                    /* conn_thread only */

/* healthybridge_rx_stats.peer_restarts as last seen by conn_thread. */
static uint32_t g_peer_epoch;

/* ---- canonical -> HealthyBridge frame mapping ---- */

static void forward_ecg(const struct hpi_sample_frame *f)
{
	const struct hp6_ecg_sample *s = f->payload;
	uint16_t n = f->sample_count;
	if (n > CONN_BATCH_MAX) {
		n = CONN_BATCH_MAX;
	}
	struct hpi_ecg_sample_multi m[CONN_BATCH_MAX];
	for (uint16_t i = 0; i < n; i++) {
		m[i].ch0     = s[i].resp;      /* respiration */
		m[i].ch1     = s[i].lead_i;
		m[i].ch2     = s[i].lead_ii;
		m[i].ch3     = s[i].v1;        /* Lead III / V1 */
		m[i].adc_ch1 = 0;
		m[i].adc_ch2 = 0;
		m[i].ppg_red = 0;              /* PPG goes on its own channel */
		m[i].ppg_ir  = 0;
	}
	uint32_t ts = (uint32_t)(f->t_mono_us / 1000ULL);
	if (healthybridge_send_ecg_multi(hb, ts, m, n) != 0) {
		g_drops++;
	} else {
		g_ecg_n++;
	}
}

static void forward_ppg(const struct hpi_sample_frame *f)
{
	const struct hp6_ppg_sample *s = f->payload;
	uint16_t n = f->sample_count;
	if (n > CONN_BATCH_MAX) {
		n = CONN_BATCH_MAX;
	}
	struct hpi_ppg_sample m[CONN_BATCH_MAX];
	for (uint16_t i = 0; i < n; i++) {
		m[i].red = s[i].red;
		m[i].ir  = s[i].ir;
	}
	uint32_t ts = (uint32_t)(f->t_mono_us / 1000ULL);
	if (healthybridge_send_ppg(hb, ts, m, n) != 0) {
		g_drops++;
	} else {
		g_ppg_n++;
	}
}

/*
 * The wire vitals record IS the canonical record (HealthyBridge 1.1.0). The
 * driver mirrors the layout because it may not include app headers; these pin
 * the two together where both are visible.
 */
BUILD_ASSERT(sizeof(struct hpi_hb_vitals_payload) == sizeof(struct hp6_vitals),
	     "HealthyBridge vitals payload must mirror struct hp6_vitals");
BUILD_ASSERT(offsetof(struct hpi_hb_vitals_payload, hrv_lf_hf_x10) ==
	     offsetof(struct hp6_vitals, hrv_lf_hf_x10) &&
	     offsetof(struct hpi_hb_vitals_payload, flags) ==
	     offsetof(struct hp6_vitals, flags),
	     "HealthyBridge vitals field offsets must mirror struct hp6_vitals");
BUILD_ASSERT(HPI_HB_VIT_HR_FROM_PPG == HP6_VIT_HR_FROM_PPG &&
	     HPI_HB_VIT_ECG_LEAD_OFF == HP6_VIT_ECG_LEAD_OFF &&
	     HPI_HB_VIT_PPG_WEAK == HP6_VIT_PPG_WEAK &&
	     HPI_HB_VIT_MOTION == HP6_VIT_MOTION,
	     "HealthyBridge vitals flags must equal HP6_VIT_*");

static void forward_vitals(const struct hpi_sample_frame *f)
{
	const struct hp6_vitals *v = f->payload;
	/* As-is: no scaling, no clamping, flags copied. A zero means "not
	 * available" on both sides of the link, so nothing needs translating. */
	struct hpi_hb_vitals_payload p = {
		.hr_bpm        = v->hr_bpm,
		.spo2_x10      = v->spo2_x10,
		.rr_bpm        = v->rr_bpm,
		.temp_c_x100   = v->temp_c_x100,
		.hrv_sdnn_ms   = v->hrv_sdnn_ms,
		.hrv_rmssd_ms  = v->hrv_rmssd_ms,
		.hrv_lf_hf_x10 = v->hrv_lf_hf_x10,
		.flags         = v->flags,
		.reserved      = 0,
	};
	if (healthybridge_send_vitals(hb, &p) != 0) {
		g_drops++;
	} else {
		g_vit_n++;
	}
}

/* ---- Link/WiFi status ----
 *
 * The transport releases its transmit lock as soon as the command is on the
 * wire and waits for the reply on a semaphore, so a slow or absent reply costs
 * this thread time and the data path nothing. The co-processor also pushes
 * unsolicited status frames, which the driver caches; wifi_status() answers
 * from that cache with no round-trip, and the poll below is only a fallback
 * for a peer that never pushes.
 *
 * Returns: 0 with real state, -ENODATA when the link is healthy but the peer's
 * status payload is still unimplemented (ESP-side item P0-4), -ENOSYS when the
 * transport has no receive path, or an error.
 */
#define CONN_WIFI_POLL_MS 2000

/*
 * Keepalive: when the link is up and this end has transmitted nothing for this
 * long, send one real GET_STATUS (healthybridge_keepalive()). At most one frame
 * per ~5 s, and only when nothing else is flowing.
 *
 * Needed because an OTA-installed C6 image keeps its slot only after it has
 * received CONFIG_HB_OTA_SELFTEST_FRAMES (3) CRC-valid host frames within
 * CONFIG_HB_OTA_SELFTEST_TIMEOUT_S (60 s) of booting, and otherwise rolls itself
 * back. With the radios off and the M4 silent this end used to send almost
 * nothing -- the 2 s status poll is answered from the driver's cache of the
 * C6's own 1 Hz push -- so an image left pending outside an update session (the
 * M7 rebooted mid-wait and something powered the C6 again) could miss it.
 * 5 s gives ~11 frames in the 60 s window.
 */
#define CONN_KEEPALIVE_IDLE_MS 5000

#define CONN_WIFI_POLL_ENABLED 1

static const char *link_state_name(uint8_t s)
{
	switch (s) {
	case HPI_CONN_LINK_OFF:      return "off";
	case HPI_CONN_LINK_STARTING: return "starting";
	case HPI_CONN_LINK_UP:       return "up";
	case HPI_CONN_LINK_FAULT:    return "fault";
	default:                     return "unknown";
	}
}

__maybe_unused static const char *wifi_state_name(uint8_t s)
{
	switch (s) {
	case HPI_WIFI_STATE_DISCONNECTED: return "disconnected";
	case HPI_WIFI_STATE_CONNECTING:   return "connecting";
	case HPI_WIFI_STATE_CONNECTED:    return "connected";
	case HPI_WIFI_STATE_AP_MODE:      return "softap";
	case HPI_WIFI_STATE_ERROR:        return "error";
	default:                          return "unknown";
	}
}

/* ---- co-processor firmware version ---- */

static void version_invalidate(const char *why)
{
	k_spinlock_key_t key = k_spin_lock(&g_ver_lock);
	bool had = (g_ver_state == CONN_VER_KNOWN);

	g_ver[0] = '\0';
	g_ver_state = CONN_VER_UNKNOWN;
	k_spin_unlock(&g_ver_lock, key);
	g_ver_tries = 0;
	if (had) {
		LOG_INF("ESP32 version forgotten (%s)", why);
	}
}

/*
 * conn_thread only, every pass while the link is powered (it is one read).
 *
 * A C6 restart -- release from reset, or an in-place reboot the driver detects
 * by the frame sequence number running backwards (panic, watchdog, the
 * esp_restart() after a firmware update or provisioning) -- invalidates
 * everything learned from the previous boot:
 *
 *   - its firmware version, which an update has just changed;
 *   - its radio state. The C6 starts with both radios off on every boot
 *     (CONFIG_HB_RADIOS_OFF_AT_BOOT), whatever it was doing before, so after
 *     an in-place reboot g_radios claims radios that are no longer running.
 *     Nothing used to notice: a unit streaming over Wi-Fi went silent after a
 *     C6 watchdog reset until someone toggled the radio by hand. Forget what
 *     was applied and let link_apply_request() re-send what was requested.
 *
 * A release from reset counts too, but there g_radios is already 0 (powering
 * down clears it), so only the version is dropped.
 */
static void peer_restart_check(void)
{
	struct healthybridge_rx_stats rx;

	if (healthybridge_get_rx_stats(hb, &rx) != 0 || rx.peer_restarts == g_peer_epoch) {
		return;
	}
	g_peer_epoch = rx.peer_restarts;
	version_invalidate("co-processor restarted");

	if (g_radios != 0) {
		LOG_INF("ESP32 restarted with radios%s%s requested -- re-applying",
			(g_req_radios & HPI_CONN_RADIO_WIFI) ? " wifi" : "",
			(g_req_radios & HPI_CONN_RADIO_BLE) ? " ble" : "");
		g_radios = 0;
		g_req_pending = true;
	}
}

/* conn_thread only. Cheap unless a fetch is due; a fetch is one round-trip,
 * bounded by the driver's command timeout. */
static void version_maintain(bool fetch_due)
{
	if (!fetch_due || g_link_state != HPI_CONN_LINK_UP ||
	    g_ver_state != CONN_VER_UNKNOWN || g_ver_tries >= CONN_VER_TRIES) {
		return;
	}
	g_ver_tries++;

	struct hpi_hb_version_resp v;
	int rc = healthybridge_get_version(hb, &v);
	k_spinlock_key_t key;

	if (rc == 0) {
		key = k_spin_lock(&g_ver_lock);
		strncpy(g_ver, v.version_string, sizeof(g_ver) - 1);
		g_ver[sizeof(g_ver) - 1] = '\0';
		g_ver_state = CONN_VER_KNOWN;
		k_spin_unlock(&g_ver_lock, key);
		LOG_INF("ESP32 firmware %s", g_ver);
	} else if (rc == -EIO || rc == -ENOSYS) {
		/* Error ack: the image has no GET_VERSION (pre-1.1.0). */
		key = k_spin_lock(&g_ver_lock);
		g_ver_state = CONN_VER_UNSUPPORTED;
		k_spin_unlock(&g_ver_lock, key);
		LOG_INF("ESP32 firmware predates GET_VERSION; version unknown");
	} else if (g_ver_tries >= CONN_VER_TRIES) {
		LOG_WRN("ESP32 did not report its version (%d); not asking again "
			"until it restarts", rc);
	}
}

/*
 * Refresh the cached co-processor status and derive the link state from
 * whether it answered. Runs only on conn_thread, and only while the link is
 * meant to be powered; the driver serves it from cache when a recent
 * unsolicited 0x61 frame has arrived. Everything the UI and the host read
 * comes from the cache this maintains -- that keeps the driver's command
 * timeout off the LVGL thread.
 */
static void link_poll(void)
{
	static uint8_t last_logged = 0xFEu;   /* neither a state nor 0xFF */
	struct hpi_hb_wifi_status_resp r = {0};
	int rc = healthybridge_wifi_status(hb, &r);

	if (rc == 0) {
		g_st = r;
		g_st.ssid[sizeof(g_st.ssid) - 1] = '\0';
		g_st_at = k_uptime_get();
		g_st_valid = true;

		if (g_link_state != HPI_CONN_LINK_UP) {
			g_link_state = HPI_CONN_LINK_UP;
			LOG_INF("ESP32 link up over %s", healthybridge_link_name(hb));
		}
		if (r.state != last_logged) {
			last_logged = r.state;
			if (r.state == HPI_WIFI_STATE_CONNECTED) {
				LOG_INF("WiFi connected: ssid='%s' ip=%u.%u.%u.%u rssi=%ddBm",
					g_st.ssid, r.ip_addr[0], r.ip_addr[1],
					r.ip_addr[2], r.ip_addr[3], r.rssi);
			} else {
				LOG_INF("WiFi %s", wifi_state_name(r.state));
			}
		}
		return;
	}

	/*
	 * -ENODATA means the co-processor acked but sent no payload -- link
	 * alive, reply body absent. Treat it as UP, not as a fault.
	 */
	if (rc == -ENODATA) {
		if (g_link_state != HPI_CONN_LINK_UP) {
			g_link_state = HPI_CONN_LINK_UP;
			LOG_INF("ESP32 link up (no status payload)");
		}
		return;
	}

	/*
	 * No answer. STARTING is not yet a fault -- the boot wait plus a first
	 * reply can legitimately take seconds (a SoftAP coming up can miss the
	 * command deadline with a healthy link). Only a link silent past its
	 * budget is called faulted.
	 */
	if (g_link_state == HPI_CONN_LINK_STARTING &&
	    (k_uptime_get() - g_st_at) < CONN_LINK_ANSWER_MS) {
		return;
	}
	if (g_link_state != HPI_CONN_LINK_FAULT) {
		g_link_state = HPI_CONN_LINK_FAULT;
		g_st_valid = false;
		last_logged = 0xFEu;
		version_invalidate("link fault");
		LOG_WRN("ESP32 not answering (%d) -- data path unaffected", rc);
	}
}

/*
 * Apply a pending enable/disable request. Runs only on conn_thread, and blocks:
 * powering the co-processor waits CONFIG_HEALTHYBRIDGE_ESP32_BOOT_DELAY_MS, and
 * each radio command is a round-trip. That is exactly why callers post an intent
 * instead of calling in.
 */
static void link_apply_request(void)
{
	bool want_power = g_req_powered;
	uint8_t want = g_req_radios;
	bool want_softap = g_req_softap;

	g_req_pending = false;
	g_req_softap = false;

	if (!want_power) {
		if (g_link_state != HPI_CONN_LINK_OFF) {
			/* Ask the radios down before cutting power, so the
			 * co-processor tears its AP/STA down in an orderly way
			 * rather than vanishing mid-association. Best-effort:
			 * if it has already stopped answering, power off anyway. */
			if (g_radios != 0) {
				(void)healthybridge_wifi_enable(hb, false);
				(void)healthybridge_ble_adv(hb, false);
			}
			(void)healthybridge_link_power(hb, false);
		}
		g_radios = 0;
		g_link_state = HPI_CONN_LINK_OFF;
		g_st_valid = false;
		memset(&g_st, 0, sizeof(g_st));
		version_invalidate("co-processor in reset");
		LOG_INF("connectivity off (co-processor in reset)");
		return;
	}

	if (g_link_state == HPI_CONN_LINK_OFF) {
		int rc = healthybridge_link_power(hb, true);

		if (rc != 0 && rc != -ENOSYS) {
			LOG_ERR("cannot power the ESP32: %d", rc);
			g_link_state = HPI_CONN_LINK_FAULT;
			return;
		}
		g_link_state = HPI_CONN_LINK_STARTING;
		g_st_at = k_uptime_get();   /* starts the answer budget */
	}

	/* Radios: only act on what changed, so a repeated request is free. */
	uint8_t changed = want ^ g_radios;

	if (changed & HPI_CONN_RADIO_WIFI) {
		int rc = healthybridge_wifi_enable(hb, (want & HPI_CONN_RADIO_WIFI) != 0);

		if (rc != 0) {
			LOG_WRN("wifi %s failed: %d",
				(want & HPI_CONN_RADIO_WIFI) ? "enable" : "disable", rc);
		}
	}
	if (changed & HPI_CONN_RADIO_BLE) {
		int rc = healthybridge_ble_adv(hb, (want & HPI_CONN_RADIO_BLE) != 0);

		if (rc != 0) {
			LOG_WRN("ble %s failed: %d",
				(want & HPI_CONN_RADIO_BLE) ? "enable" : "disable", rc);
		}
	}
	g_radios = want;

	if (want_softap) {
		int rc = healthybridge_wifi_softap(hb);

		LOG_INF("provisioning portal %s (%d)", rc == 0 ? "requested" : "FAILED", rc);
	}

	LOG_INF("connectivity: link %s, radios%s%s",
		link_state_name(g_link_state),
		(g_radios & HPI_CONN_RADIO_WIFI) ? " wifi" : "",
		(g_radios & HPI_CONN_RADIO_BLE) ? " ble" : "");
}

/* ---- bus consumer thread ---- */

static void conn_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	struct hpi_sample_frame f;
#if CONN_WIFI_POLL_ENABLED
	int64_t wpoll = k_uptime_get();
#endif
#if IS_ENABLED(CONFIG_HPI_CONN_DEBUG)
	int64_t win = k_uptime_get();
#endif
	while (1) {
		if (g_req_pending) {
			link_apply_request();
		}

		/* 1s pull timeout so the poll/stats windows still tick if acq stalls */
		if (hpi_bus_pull_wait(g_sub, &f, 1000) == 0) {
			/*
			 * Frames are pulled even while the link is off, and
			 * discarded (counted as skipped): the ring must keep
			 * draining or its dropped counter -- the link-health
			 * number -- inflates.
			 *
			 * Vitals exception below: an OTA-installed C6 image
			 * keeps its slot only after it receives 3 CRC-valid
			 * frames from this host within 60 s of booting
			 * (CONFIG_HB_OTA_SELFTEST_FRAMES / _TIMEOUT_S on the C6).
			 * Vitals, waveforms, status polls and control commands
			 * all count; Wi-Fi being up does not. With the radios
			 * off the ~1 Hz vitals are what keeps a radios-off boot
			 * from rolling itself back -- and they exist only while
			 * the M4 is producing. Without them the status poll is
			 * mostly answered from the driver's cache of the C6's
			 * 1 Hz status push, i.e. not sent at all; the keepalive
			 * below (CONN_KEEPALIVE_IDLE_MS) is what covers that.
			 */
			bool live = (g_link_state == HPI_CONN_LINK_UP ||
				     g_link_state == HPI_CONN_LINK_STARTING);

			if (!live) {
				g_skipped++;
			} else if (g_radios == 0 && f.channel != HPI_CH_VITALS) {
				/* Powered but no radio: nothing is listening, so
				 * waveforms are pure waste. Keep the 1 Hz vitals
				 * keepalive. */
				g_skipped++;
			} else {
				switch (f.channel) {
				case HPI_CH_ECG:    forward_ecg(&f);    break;
				case HPI_CH_PPG:    forward_ppg(&f);    break;
				case HPI_CH_VITALS: forward_vitals(&f); break;
				default: break;
				}
			}
		}
		int64_t now = k_uptime_get();

#if CONN_WIFI_POLL_ENABLED
		/* Only poll a link that is meant to be powered. Polling one held
		 * in reset would burn the driver's command timeout every window,
		 * forever, for an answer that cannot come. */
		bool poll_due = (g_link_state != HPI_CONN_LINK_OFF &&
				 now - wpoll >= CONN_WIFI_POLL_MS);

		if (poll_due) {
			link_poll();
			wpoll = now;
		}
#else
		bool poll_due = (g_link_state != HPI_CONN_LINK_OFF);
#endif
		/* Restart detection every pass (it is two reads); the fetch
		 * rides the poll cadence so retries are a window apart. */
		if (g_link_state != HPI_CONN_LINK_OFF) {
			peer_restart_check();
			version_maintain(poll_due);
		}
		/* Cheap: a timestamp compare unless the link has been quiet. */
		if (g_link_state == HPI_CONN_LINK_UP) {
			(void)healthybridge_keepalive(hb, CONN_KEEPALIVE_IDLE_MS);
		}
#if IS_ENABLED(CONFIG_HPI_CONN_DEBUG)
		if (now - win >= 5000) {
			struct healthybridge_rx_stats rx;

			if (healthybridge_get_rx_stats(hb, &rx) == 0) {
				/* Cumulative since boot, unlike the TX counters
				 * above which are per-window -- the RX side is
				 * sparse enough that a rate would read as noise. */
				LOG_INF("conn 5s [%s]: ECG=%u PPG=%u vitals=%u sent"
					" | drops=%u skipped=%u"
					" | rx %uB/%ufr crc_err=%u resync=%u ovf=%u stop=%u",
					link_state_name(g_link_state),
					g_ecg_n, g_ppg_n, g_vit_n, g_drops, g_skipped,
					rx.bytes, rx.frames, rx.crc_err, rx.resync,
					rx.ovf, rx.stopped);
			} else {
				LOG_INF("conn 5s [%s]: ECG=%u PPG=%u vitals=%u sent"
					" | drops=%u skipped=%u",
					link_state_name(g_link_state),
					g_ecg_n, g_ppg_n, g_vit_n, g_drops, g_skipped);
			}
			g_ecg_n = g_ppg_n = g_vit_n = g_drops = g_skipped = 0;
			win = now;
		}
#endif
	}
}

K_THREAD_STACK_DEFINE(conn_stack, 4096);
static struct k_thread conn_thr;

void hpi_connectivity_init(void)
{
	if (!device_is_ready(hb)) {
		LOG_WRN("HealthyBridge ESP32 not ready -- WiFi/BLE offline");
		return;
	}
	struct hpi_bus_sub_cfg cfg = {
		.name = "conn",
		.channel_mask = HPI_CH_BIT(HPI_CH_ECG) | HPI_CH_BIT(HPI_CH_PPG) |
				HPI_CH_BIT(HPI_CH_VITALS),
		.ring_frames = 16,
	};
	g_sub = hpi_bus_subscribe(&cfg);
	if (g_sub == NULL) {
		LOG_ERR("connectivity: bus subscribe failed");
		return;
	}
	k_thread_create(&conn_thr, conn_stack, K_THREAD_STACK_SIZEOF(conn_stack),
			conn_thread, NULL, NULL, NULL, 7, 0, K_NO_WAIT);
	k_thread_name_set(&conn_thr, "hpi_conn");
	g_started = true;
	LOG_INF("connectivity: service up over %s; radios off until enabled",
		healthybridge_link_name(hb));
}

/* ---- enable / disable / status ---- */

int hpi_connectivity_enable(uint8_t radio_mask)
{
	if (!g_started) {
		return -ENODEV;
	}
	if (radio_mask & ~(HPI_CONN_RADIO_WIFI | HPI_CONN_RADIO_BLE)) {
		return -EINVAL;
	}
	g_req_radios = radio_mask;
	g_req_powered = true;
	g_req_pending = true;
	return 0;
}

void hpi_connectivity_disable(void)
{
	if (!g_started) {
		return;
	}
	g_req_radios = 0;
	g_req_powered = false;
	g_req_softap = false;
	g_req_pending = true;
}

void hpi_connectivity_get_status(struct hpi_conn_status *out)
{
	if (out == NULL) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->link_state = g_link_state;
	out->radios = g_req_radios;
	out->frames_sent = g_ecg_n + g_ppg_n + g_vit_n;
	out->frames_dropped = g_drops;

	if (g_st_valid) {
		out->wifi_state = g_st.state;
		out->rssi = g_st.rssi;
		memcpy(out->ip, g_st.ip_addr, sizeof(out->ip));
		memcpy(out->ssid, g_st.ssid, sizeof(g_st.ssid));
		out->ssid[sizeof(out->ssid) - 1] = '\0';
		out->ble_adv = (g_st.ble_adv != 0);
		out->ble_conn = (g_st.ble_conn != 0);
	}
}

int hpi_connectivity_esp_version(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	if (!g_started) {
		return -ENODEV;
	}
	if (g_link_state == HPI_CONN_LINK_OFF) {
		return -EHOSTDOWN;
	}

	k_spinlock_key_t key = k_spin_lock(&g_ver_lock);
	uint8_t st = g_ver_state;

	if (st == CONN_VER_KNOWN) {
		strncpy(out, g_ver, cap - 1);
		out[cap - 1] = '\0';
	}
	k_spin_unlock(&g_ver_lock, key);

	switch (st) {
	case CONN_VER_KNOWN:       return 0;
	case CONN_VER_UNSUPPORTED: return -ENOTSUP;
	default:                   return -EAGAIN;
	}
}

bool hpi_connectivity_ready(void)
{
	/* "The co-processor is powered and answering", not "the service thread
	 * started" -- the status-bar link glyph reads this and must not light
	 * for an absent or deliberately powered-down co-processor. */
	return g_link_state == HPI_CONN_LINK_UP;
}

/* ---- compatibility wrappers ----
 *
 * All of these answer from the cache or post an intent; none may issue a
 * round-trip. Callers include the LVGL thread and the SMP handlers, and a
 * round-trip costs the driver's full command timeout when the co-processor is
 * slow, absent, or off.
 */

int hpi_connectivity_wifi_status(struct hpi_wifi_info *out)
{
	if (!device_is_ready(hb)) {
		return -ENODEV;
	}
	/* -EHOSTDOWN = off because that was asked for (a normal state, never a
	 * fault); -ENODATA/-ETIMEDOUT = should be answering and is not. */
	if (g_link_state == HPI_CONN_LINK_OFF) {
		return -EHOSTDOWN;
	}
	if (!g_st_valid) {
		return (g_link_state == HPI_CONN_LINK_STARTING) ? -EAGAIN : -ETIMEDOUT;
	}
	if (out) {
		out->state = g_st.state;
		out->rssi  = g_st.rssi;
		memcpy(out->ip, g_st.ip_addr, sizeof(out->ip));
		memcpy(out->ssid, g_st.ssid, sizeof(g_st.ssid));
		out->ssid[sizeof(out->ssid) - 1] = '\0';
	}
	return 0;
}

int hpi_connectivity_wifi_connect(const char *ssid, const char *password)
{
	if (!device_is_ready(hb)) {
		return -ENODEV;
	}
	/* Still -ENOTSUP at the transport: there is no command that carries
	 * credentials. Provisioning is the SoftAP portal, by design. */
	return healthybridge_wifi_connect(hb, ssid, password);
}

int hpi_connectivity_wifi_disconnect(void)
{
	if (!g_started) {
		return -ENODEV;
	}
	return hpi_connectivity_enable(g_req_radios & ~HPI_CONN_RADIO_WIFI);
}

int hpi_connectivity_wifi_enable(bool on)
{
	if (!g_started) {
		return -ENODEV;
	}
	uint8_t m = g_req_radios;

	if (on) {
		m |= HPI_CONN_RADIO_WIFI;
	} else {
		m &= ~HPI_CONN_RADIO_WIFI;
	}
	/* Turning the last radio off powers the co-processor down; keeping it
	 * powered with nothing running is available via hpi_connectivity_enable(0). */
	if (m == 0) {
		hpi_connectivity_disable();
		return 0;
	}
	return hpi_connectivity_enable(m);
}

int hpi_connectivity_ble_enable(bool on)
{
	if (!g_started) {
		return -ENODEV;
	}
	uint8_t m = g_req_radios;

	if (on) {
		m |= HPI_CONN_RADIO_BLE;
	} else {
		m &= ~HPI_CONN_RADIO_BLE;
	}
	if (m == 0) {
		hpi_connectivity_disable();
		return 0;
	}
	return hpi_connectivity_enable(m);
}

int hpi_connectivity_wifi_softap(void)
{
	if (!g_started) {
		return -ENODEV;
	}
	/* The portal is how a device with no credentials gets any, so it powers
	 * the co-processor and lifts the WiFi gate itself; the thread issues the
	 * SOFTAP command once the link is powered. */
	g_req_softap = true;
	return hpi_connectivity_enable(g_req_radios | HPI_CONN_RADIO_WIFI);
}

#else /* !HB_AVAILABLE -- connectivity off: no-op stubs */

void hpi_connectivity_init(void) { }
bool hpi_connectivity_ready(void) { return false; }
int hpi_connectivity_esp_version(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return -EINVAL;
	}
	out[0] = '\0';
	return -ENOTSUP;
}
int hpi_connectivity_enable(uint8_t radio_mask) { ARG_UNUSED(radio_mask); return -ENOTSUP; }
void hpi_connectivity_disable(void) { }
void hpi_connectivity_get_status(struct hpi_conn_status *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
		out->link_state = HPI_CONN_LINK_OFF;
	}
}
int hpi_connectivity_wifi_status(struct hpi_wifi_info *out) { ARG_UNUSED(out); return -ENOTSUP; }
int hpi_connectivity_wifi_connect(const char *ssid, const char *password)
{
	ARG_UNUSED(ssid); ARG_UNUSED(password); return -ENOTSUP;
}
int hpi_connectivity_wifi_disconnect(void) { return -ENOTSUP; }
int hpi_connectivity_wifi_enable(bool on) { ARG_UNUSED(on); return -ENOTSUP; }
int hpi_connectivity_ble_enable(bool on) { ARG_UNUSED(on); return -ENOTSUP; }
int hpi_connectivity_wifi_softap(void) { return -ENOTSUP; }

#endif /* HB_AVAILABLE */
