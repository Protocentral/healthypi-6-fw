/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * ESP32-C6 firmware update service -- see esp32_update_service.h.
 *
 * Who owns the session state when:
 *
 *   IDLE / terminal  begin() (SMP thread) starts a session      -> PREPARING
 *   PREPARING        espfw thread: hold the link, STATUS, BEGIN -> RECEIVING
 *   RECEIVING        chunk() / commit() / abort() (SMP thread) and the thread's
 *                    host-idle check, serialised by g_mx         -> COMMITTING
 *   COMMITTING ..    espfw thread only: COMMIT, then watch the
 *   VERIFYING        restart and the self-test                   -> terminal
 *
 * g_mx serialises exchanges on the link and the RECEIVING-state fields; it is
 * never held across a wait for the link to come up. g_lk guards the snapshot
 * that status() reads, so status never waits on the link -- not even during
 * the up-to-10 s COMMIT reply.
 */

#include "esp32_update_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include <healthybridge_esp32_link.h>

#if IS_ENABLED(CONFIG_HPI_ESP32_UPDATE) && defined(HEALTHYBRIDGE_LINK_NODE)

#include <psa/crypto.h>

#include "fw_release_sig.h"
#include "services/connectivity/healthybridge_service.h"

LOG_MODULE_REGISTER(hpi_espfw, CONFIG_HPI_APP_LOG_LEVEL);

BUILD_ASSERT(HPI_ESPFW_SIG_LEN == HPI_FW_SIG_LEN &&
	     HPI_ESPFW_SHA256_LEN == HPI_FW_SHA256_LEN,
	     "the C6 image is signed exactly like the M4 image");
BUILD_ASSERT(HPI_ESPFW_MAX_CHUNK == HPI_HB_OTA_MAX_CHUNK,
	     "one chunk is one HealthyBridge DATA frame");
BUILD_ASSERT(HPI_ESPFW_UNKNOWN == HPI_HB_OTA_IMG_UNKNOWN,
	     "unknown is spelled the same on both sides");

static const struct device *const hb = DEVICE_DT_GET(HEALTHYBRIDGE_LINK_NODE);

/*
 * Timing, from docs/PROTOCOL.md "Timing" (healthybridge-esp32):
 *   BEGIN / DATA / ABORT / STATUS reply   bound ~0.45 s, wait HPI_ESPFW_REPLY_MS
 *   COMMIT reply                          ~1 s typical,  wait 10 s
 *   restart to link up                    allow 10 s
 *   link up to VALID                      3 host frames; allow 75 s
 *
 * The VALID budget must OUTLAST the C6's own self-test window
 * (CONFIG_HB_OTA_SELFTEST_TIMEOUT_S, 60 s in healthybridge-esp32): only then
 * does a failing image end this watch as ROLLED_BACK, seen by evaluate(),
 * instead of a TIMEOUT that hides the C6's verdict. At 20 s the M7 gave up
 * while the C6 was still deciding (bench 2026-11-12: status stuck at
 * TIMEOUT/NOT_VALID after the C6 had rolled back by itself).
 */
#define ESPFW_COMMIT_REPLY_MS  10000U
#define ESPFW_LINK_UP_MS        8000U   /* power-up (0.8 s) + first answer */
#define ESPFW_HOST_IDLE_MS     30000U   /* = the C6's own inactivity abort */
#define ESPFW_REBOOT_MS        10000U
#define ESPFW_VALID_MS         75000U   /* > the C6's 60 s self-test window */
#define ESPFW_TICK_MS           1000U   /* STATUS poll cadence while watching */
#define ESPFW_PENDING_POLL_MS   2000U   /* idle re-poll of a PENDING image */
#define ESPFW_TRIES                3    /* STATUS / BEGIN attempts in PREPARING */

static struct k_spinlock g_lk;          /* g_st */
static struct hpi_espfw_status g_st = {
	.c6_state = HPI_ESPFW_UNKNOWN, .img = HPI_ESPFW_UNKNOWN,
	.other = HPI_ESPFW_UNKNOWN, .slot = HPI_ESPFW_UNKNOWN,
};
static K_MUTEX_DEFINE(g_mx);
static K_SEM_DEFINE(g_kick, 0, 1);
static atomic_t g_abort_req;

/* Session. RECEIVING-state fields under g_mx; otherwise owned per the table at
 * the top of this file. */
static struct {
	uint8_t  sha[HPI_ESPFW_SHA256_LEN];
	uint32_t size;
	uint32_t next;            /* next offset; = bytes relayed and acknowledged */
	uint32_t last_off;        /* the chunk accepted last, for idempotent resend */
	uint16_t last_len;
	bool     hashing;
	psa_hash_operation_t hash;
	int64_t  last_chunk_at;
	uint8_t  slot_before;     /* C6 running slot when the session began */
	bool     commit_unconfirmed;
	int64_t  deadline;
} s;

/* C6 image info outside a session: refreshed once per C6 boot, and every
 * ESPFW_PENDING_POLL_MS while its image is pending. */
static bool     g_info_valid;
static int64_t  g_info_at;
static uint32_t g_info_epoch;
static bool     g_noota;            /* this boot of the C6 does not speak OTA */
static uint32_t g_noota_epoch;

/* ---- snapshot helpers ---- */

static const char *state_name(uint8_t st)
{
	static const char *const n[] = {
		"idle", "preparing", "receiving", "committing", "rebooting",
		"verifying", "done", "rolled-back", "failed", "timeout",
	};
	return st < ARRAY_SIZE(n) ? n[st] : "?";
}

static uint8_t get_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);
	uint8_t st = g_st.state;

	k_spin_unlock(&g_lk, key);
	return st;
}

static bool session_active(uint8_t st)
{
	return st >= HPI_ESPFW_PREPARING && st <= HPI_ESPFW_VERIFYING;
}

static void set_state(uint8_t st, uint8_t err)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);
	uint8_t was = g_st.state;

	g_st.state = st;
	g_st.err = err;
	k_spin_unlock(&g_lk, key);

	if (was != st) {
		if (err != HPI_ESPFW_ERR_NONE) {
			LOG_WRN("ESP32 update: %s -> %s (err %u)", state_name(was),
				state_name(st), err);
		} else {
			LOG_INF("ESP32 update: %s -> %s", state_name(was), state_name(st));
		}
	}
}

static void note_c6_err(uint8_t code)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	g_st.c6_err = code;
	k_spin_unlock(&g_lk, key);
	LOG_WRN("ESP32 update: C6 refused (status 0x%02x)", code);
}

static void set_relayed(uint32_t n)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	g_st.relayed = n;
	k_spin_unlock(&g_lk, key);
}

static void absorb_status(const struct hpi_hb_ota_status *st)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	g_st.c6_state = st->state;
	g_st.img = st->running_img_state;
	g_st.other = st->other_img_state;
	g_st.slot = st->running_slot;
	memcpy(g_st.version, st->version, sizeof(g_st.version));
	g_st.version[sizeof(g_st.version) - 1] = '\0';
	k_spin_unlock(&g_lk, key);
}

static void forget_c6_info(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	g_st.c6_state = HPI_ESPFW_UNKNOWN;
	g_st.img = HPI_ESPFW_UNKNOWN;
	g_st.other = HPI_ESPFW_UNKNOWN;
	g_st.slot = HPI_ESPFW_UNKNOWN;
	g_st.version[0] = '\0';
	k_spin_unlock(&g_lk, key);
	g_info_valid = false;
}

/* ---- link helpers (caller holds g_mx) ---- */

static int c6_status(struct hpi_hb_ota_reply *r)
{
	int rc = healthybridge_ota_request(hb, HPI_HB_OTA_OP_STATUS, NULL, 0, NULL, 0,
					   HPI_ESPFW_REPLY_MS, r);

	if (rc == 0) {
		absorb_status(&r->u.st);
		g_info_valid = true;
		g_info_at = k_uptime_get();
	}
	return rc;
}

/* Best effort: ABORT is always safe before COMMIT and always answered OK. */
static void c6_abort(void)
{
	struct hpi_hb_ota_reply r;
	int rc = healthybridge_ota_request(hb, HPI_HB_OTA_OP_ABORT, NULL, 0, NULL, 0,
					   HPI_ESPFW_REPLY_MS, &r);

	if (rc != 0) {
		LOG_WRN("ESP32 update: ABORT not acknowledged (%d); the C6 drops the "
			"transfer itself after 30 s", rc);
	}
}

static void hash_drop(void)
{
	if (s.hashing) {
		(void)psa_hash_abort(&s.hash);
		s.hashing = false;
	}
}

/*
 * End the session and hand the link back. keep_powered only for an outcome
 * that could leave a PENDING image behind: putting the C6 back into reset then
 * would roll it back.
 */
static void finish(uint8_t st, uint8_t err, bool keep_powered)
{
	hash_drop();
	set_state(st, err);
	hpi_connectivity_update_release(keep_powered);
}

/* ---- thread steps ---- */

static void do_prepare(void)
{
	struct hpi_hb_ota_reply r;
	int64_t until = k_uptime_get() + ESPFW_LINK_UP_MS;
	int rc;

	while (!hpi_connectivity_ready()) {
		if (atomic_get(&g_abort_req)) {
			finish(HPI_ESPFW_IDLE, HPI_ESPFW_ERR_ABORTED, false);
			return;
		}
		if (k_uptime_get() >= until) {
			LOG_ERR("ESP32 update: the C6 did not come up");
			finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_LINK, false);
			return;
		}
		k_msleep(100);
	}

	k_mutex_lock(&g_mx, K_FOREVER);

	rc = -ETIMEDOUT;
	for (int i = 0; i < ESPFW_TRIES && rc == -ETIMEDOUT; i++) {
		rc = c6_status(&r);
	}
	if (rc != 0) {
		k_mutex_unlock(&g_mx);
		/* It answers GET_STATUS (the link is up) but not OTA STATUS: an
		 * image from before the update path existed. */
		LOG_ERR("ESP32 update: no OTA STATUS reply (%d) -- C6 firmware "
			"predates the update path?", rc);
		finish(HPI_ESPFW_FAILED, rc == -ETIMEDOUT ? HPI_ESPFW_ERR_NO_OTA
							  : HPI_ESPFW_ERR_LINK, false);
		return;
	}
	s.slot_before = r.u.st.running_slot;
	LOG_INF("ESP32 update: C6 runs %s from slot %u (%u B slot free)",
		r.u.st.version, r.u.st.running_slot, r.u.st.slot_size);

	struct hpi_hb_ota_begin b = { .image_size = s.size };

	memcpy(b.sha256, s.sha, sizeof(b.sha256));
	/* An identical BEGIN restarts the transfer at 0, so resending one whose
	 * reply was lost is harmless. */
	rc = -ETIMEDOUT;
	for (int i = 0; i < ESPFW_TRIES && rc == -ETIMEDOUT; i++) {
		rc = healthybridge_ota_request(hb, HPI_HB_OTA_OP_BEGIN, &b, 0, NULL, 0,
					       HPI_ESPFW_REPLY_MS, &r);
	}

	if (rc == 0 && r.status == HPI_HB_OTA_OK && r.u.ack.next_offset == 0 &&
	    r.u.ack.image_size == s.size) {
		if (atomic_get(&g_abort_req)) {
			c6_abort();
			k_mutex_unlock(&g_mx);
			finish(HPI_ESPFW_IDLE, HPI_ESPFW_ERR_ABORTED, false);
			return;
		}
		s.last_chunk_at = k_uptime_get();
		set_state(HPI_ESPFW_RECEIVING, HPI_ESPFW_ERR_NONE);
		k_mutex_unlock(&g_mx);
		return;
	}

	uint8_t err;

	if (rc == 0 && r.status != HPI_HB_OTA_OK) {
		note_c6_err(r.status);   /* nothing started on the C6 */
		err = HPI_ESPFW_ERR_C6_REFUSED;
	} else if (rc == 0) {
		c6_abort();
		err = HPI_ESPFW_ERR_PROTOCOL;
	} else if (rc == -ETIMEDOUT) {
		c6_abort();   /* in case a BEGIN did land */
		err = HPI_ESPFW_ERR_NO_REPLY;
	} else {
		err = HPI_ESPFW_ERR_LINK;
	}
	k_mutex_unlock(&g_mx);
	finish(HPI_ESPFW_FAILED, err, false);
}

static void do_host_idle_check(void)
{
	k_mutex_lock(&g_mx, K_FOREVER);
	if (get_state() == HPI_ESPFW_RECEIVING &&
	    k_uptime_get() - s.last_chunk_at >= ESPFW_HOST_IDLE_MS) {
		LOG_WRN("ESP32 update: no chunk for %u s -- aborting",
			ESPFW_HOST_IDLE_MS / 1000U);
		c6_abort();
		k_mutex_unlock(&g_mx);
		finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_HOST_IDLE, false);
		return;
	}
	k_mutex_unlock(&g_mx);
}

/* COMMIT is sent exactly once. Without a reply the outcome is read from
 * STATUS, never by resending it (docs/PROTOCOL.md, Rules). */
static void do_commit(void)
{
	struct hpi_hb_ota_reply r;
	int rc;

	k_mutex_lock(&g_mx, K_FOREVER);
	rc = healthybridge_ota_request(hb, HPI_HB_OTA_OP_COMMIT, NULL, 0, NULL, 0,
				       ESPFW_COMMIT_REPLY_MS, &r);
	k_mutex_unlock(&g_mx);

	if (rc == 0 && r.status == HPI_HB_OTA_OK) {
		LOG_INF("ESP32 update: COMMIT accepted -- C6 restarting into the new image");
		s.commit_unconfirmed = false;
	} else if (rc == 0) {
		/* Every COMMIT failure on the C6 leaves its boot slot unchanged. */
		note_c6_err(r.status);
		finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_C6_REFUSED, false);
		return;
	} else {
		LOG_WRN("ESP32 update: no COMMIT reply (%d) -- reading the outcome "
			"from STATUS", rc);
		s.commit_unconfirmed = true;
	}
	s.deadline = k_uptime_get() + ESPFW_REBOOT_MS;
	set_state(HPI_ESPFW_REBOOTING, HPI_ESPFW_ERR_NONE);
}

/* A STATUS from a C6 that has restarted since COMMIT: which image is it on? */
static void evaluate(const struct hpi_hb_ota_status *st)
{
	if (st->running_slot != s.slot_before) {
		if (st->running_img_state == HPI_HB_OTA_IMG_VALID) {
			LOG_INF("ESP32 update: %s running from slot %u and VALID",
				st->version, st->running_slot);
			finish(HPI_ESPFW_DONE, HPI_ESPFW_ERR_NONE, false);
		} else if (get_state() != HPI_ESPFW_VERIFYING) {
			LOG_INF("ESP32 update: %s up from slot %u, pending its self-test",
				st->version, st->running_slot);
			s.deadline = k_uptime_get() + ESPFW_VALID_MS;
			set_state(HPI_ESPFW_VERIFYING, HPI_ESPFW_ERR_NONE);
		}
		return;
	}
	if (st->other_img_state == HPI_HB_OTA_IMG_INVALID ||
	    st->other_img_state == HPI_HB_OTA_IMG_ABORTED) {
		LOG_ERR("ESP32 update: ROLLED BACK -- %s running from slot %u, new "
			"image state %u", st->version, st->running_slot, st->other_img_state);
		finish(HPI_ESPFW_ROLLED_BACK, HPI_ESPFW_ERR_ROLLED_BACK, false);
		return;
	}
	/* Restarted on the old slot with no rollback recorded: the switch never
	 * happened. The old image is running and valid, so a reset is safe. */
	LOG_ERR("ESP32 update: C6 restarted on its old image (slot %u) without "
		"switching", st->running_slot);
	finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_NOT_SWITCHED, false);
}

/* One poll while REBOOTING or VERIFYING. Each poll is also one of the host
 * frames the new image's self-test is waiting for. */
static void do_watch(void)
{
	struct hpi_hb_ota_reply r;
	uint8_t st_now = get_state();
	int rc;

	k_mutex_lock(&g_mx, K_FOREVER);
	rc = c6_status(&r);
	k_mutex_unlock(&g_mx);

	if (rc == 0) {
		const struct hpi_hb_ota_status *st = &r.u.st;

		if (st_now == HPI_ESPFW_VERIFYING) {
			evaluate(st);
		} else if (st->state == HPI_HB_OTA_STATE_VERIFYING ||
			   st->state == HPI_HB_OTA_STATE_COMMITTED) {
			/* Still the old image, hashing or about to restart. */
		} else if (s.commit_unconfirmed &&
			   st->running_slot == s.slot_before &&
			   (st->state == HPI_HB_OTA_STATE_RECEIVING ||
			    st->state == HPI_HB_OTA_STATE_ERROR)) {
			/* The COMMIT never ran (RECEIVING: it was lost on the
			 * wire) or ran and failed (ERROR). Boot slot unchanged
			 * either way, and COMMIT is never resent. */
			k_mutex_lock(&g_mx, K_FOREVER);
			c6_abort();
			k_mutex_unlock(&g_mx);
			if (st->state == HPI_HB_OTA_STATE_ERROR) {
				note_c6_err(st->last_error);
				finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_C6_REFUSED, false);
			} else {
				finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_NO_REPLY, false);
			}
			return;
		} else {
			evaluate(st);   /* restarted (state IDLE on a fresh boot) */
		}
	}

	st_now = get_state();
	if (session_active(st_now) && k_uptime_get() >= s.deadline) {
		if (st_now == HPI_ESPFW_REBOOTING) {
			LOG_ERR("ESP32 update: the C6 did not come back within %u s",
				ESPFW_REBOOT_MS / 1000U);
			finish(HPI_ESPFW_TIMEOUT, HPI_ESPFW_ERR_NOT_BACK, true);
		} else {
			LOG_ERR("ESP32 update: new image not VALID within %u s",
				ESPFW_VALID_MS / 1000U);
			finish(HPI_ESPFW_TIMEOUT, HPI_ESPFW_ERR_NOT_VALID, true);
		}
	}
}

/*
 * A session that ended in TIMEOUT left the C6 powered with its outcome
 * unknown. If it turns up later -- the new image VALID, or the old one back
 * with the new one marked bad -- record that instead, so the host polling
 * espfw_status sees the real result rather than a stale timeout.
 */
static void late_outcome(const struct hpi_hb_ota_status *st)
{
	if (get_state() != HPI_ESPFW_TIMEOUT || st->running_slot == HPI_ESPFW_UNKNOWN) {
		return;
	}
	if (st->running_slot != s.slot_before &&
	    st->running_img_state == HPI_HB_OTA_IMG_VALID) {
		set_state(HPI_ESPFW_DONE, HPI_ESPFW_ERR_NONE);
	} else if (st->running_slot == s.slot_before &&
		   (st->other_img_state == HPI_HB_OTA_IMG_INVALID ||
		    st->other_img_state == HPI_HB_OTA_IMG_ABORTED)) {
		set_state(HPI_ESPFW_ROLLED_BACK, HPI_ESPFW_ERR_ROLLED_BACK);
	}
}

/*
 * Outside a session: learn the C6's image state once per C6 boot (and keep
 * re-reading it while it is PENDING_VERIFY -- those reads are host frames for
 * its self-test too). This is what makes hpi_esp32_update_busy() true for a
 * pending image the M7 did not install in this boot.
 */
static void do_idle_refresh(void)
{
	struct healthybridge_rx_stats rx;
	struct hpi_hb_ota_reply r;

	if (!hpi_connectivity_ready()) {
		if (g_info_valid) {
			forget_c6_info();
		}
		return;
	}
	if (hpi_connectivity_update_held() ||
	    healthybridge_get_rx_stats(hb, &rx) != 0) {
		return;
	}

	bool restarted = (rx.peer_restarts != g_info_epoch);
	bool pending;
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	pending = (g_st.img == HPI_HB_OTA_IMG_PENDING_VERIFY);
	k_spin_unlock(&g_lk, key);

	if (g_noota && g_noota_epoch == rx.peer_restarts) {
		return;   /* asked this boot; it does not speak OTA */
	}
	if (g_info_valid && !restarted &&
	    !(pending && k_uptime_get() - g_info_at >= ESPFW_PENDING_POLL_MS)) {
		return;
	}

	k_mutex_lock(&g_mx, K_FOREVER);
	int rc = c6_status(&r);

	k_mutex_unlock(&g_mx);
	if (rc == 0) {
		g_info_epoch = rx.peer_restarts;
		g_noota = false;
		late_outcome(&r.u.st);
		if (restarted && r.u.st.running_img_state == HPI_HB_OTA_IMG_PENDING_VERIFY) {
			LOG_WRN("ESP32 runs %s PENDING verification -- keeping it "
				"talking; resets are refused until it is VALID",
				r.u.st.version);
		}
	} else if (rc == -ETIMEDOUT) {
		g_noota = true;
		g_noota_epoch = rx.peer_restarts;
		forget_c6_info();
	}
}

static void espfw_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&g_kick, K_MSEC(ESPFW_TICK_MS));

		switch (get_state()) {
		case HPI_ESPFW_PREPARING:  do_prepare();         break;
		case HPI_ESPFW_RECEIVING:  do_host_idle_check(); break;
		case HPI_ESPFW_COMMITTING: do_commit();          break;
		case HPI_ESPFW_REBOOTING:
		case HPI_ESPFW_VERIFYING:  do_watch();           break;
		default:                   do_idle_refresh();    break;
		}
	}
}

/*
 * Stack: the deepest path is a STATUS exchange (a ~60 B reply on the stack)
 * plus logging; no crypto runs here -- hashing and the signature check happen
 * on the SMP thread, inside chunk() and begin().
 */
K_THREAD_STACK_DEFINE(espfw_stack, 1536);
static struct k_thread espfw_thr;
static bool g_started;

void hpi_esp32_update_init(void)
{
	if (!device_is_ready(hb)) {
		LOG_WRN("ESP32 update: no co-processor link");
		return;
	}
	k_thread_create(&espfw_thr, espfw_stack, K_THREAD_STACK_SIZEOF(espfw_stack),
			espfw_thread, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&espfw_thr, "hpi_espfw");
	g_started = true;
}

/* ---- public API ---- */

int hpi_esp32_update_begin(uint32_t size, const uint8_t sha256[HPI_ESPFW_SHA256_LEN],
			   const uint8_t *sig, size_t sig_len)
{
	if (size == 0U || sha256 == NULL || sig == NULL || sig_len != HPI_ESPFW_SIG_LEN) {
		return -EINVAL;
	}
	if (size > HPI_ESPFW_MAX_IMAGE) {
		return -ENOSPC;
	}
	if (!g_started) {
		return -ENODEV;
	}
	if (hpi_esp32_update_busy()) {
		/* A session is running, or the C6 is still proving the image a
		 * previous one installed (it would refuse BEGIN anyway). */
		return -EBUSY;
	}

	/* (c) first: a forged or corrupt signature costs no transfer and never
	 * wakes the C6. */
	int rc = hpi_fw_release_sig_verify(sha256, sig);

	if (rc != 0) {
		k_spinlock_key_t key = k_spin_lock(&g_lk);

		g_st.size = size;
		g_st.relayed = 0;
		g_st.c6_err = 0;
		k_spin_unlock(&g_lk, key);
		set_state(HPI_ESPFW_FAILED, rc == -EBADMSG ? HPI_ESPFW_ERR_SIG_INVALID
							   : HPI_ESPFW_ERR_CRYPTO);
		return rc == -EBADMSG ? -EBADMSG : -EIO;
	}

	k_mutex_lock(&g_mx, K_FOREVER);
	if (session_active(get_state())) {
		k_mutex_unlock(&g_mx);
		return -EBUSY;
	}
	hash_drop();
	if (psa_crypto_init() != PSA_SUCCESS) {
		k_mutex_unlock(&g_mx);
		return -EIO;
	}
	s.hash = psa_hash_operation_init();
	if (psa_hash_setup(&s.hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
		k_mutex_unlock(&g_mx);
		return -EIO;
	}
	s.hashing = true;
	memcpy(s.sha, sha256, sizeof(s.sha));
	s.size = size;
	s.next = 0;
	s.last_off = 0;
	s.last_len = 0;
	s.commit_unconfirmed = false;
	atomic_clear(&g_abort_req);

	k_spinlock_key_t key = k_spin_lock(&g_lk);

	g_st.size = size;
	g_st.relayed = 0;
	g_st.c6_err = 0;
	k_spin_unlock(&g_lk, key);
	set_state(HPI_ESPFW_PREPARING, HPI_ESPFW_ERR_NONE);
	k_mutex_unlock(&g_mx);

	rc = hpi_connectivity_update_hold();
	if (rc != 0) {
		finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_LINK, false);
		return -ENODEV;
	}
	LOG_INF("ESP32 update: begin, %u B, signature OK", size);
	k_sem_give(&g_kick);
	return 0;
}

int hpi_esp32_update_chunk(uint32_t offset, const uint8_t *data, size_t len,
			   uint32_t *next)
{
	struct hpi_hb_ota_reply r;
	int rc;

	if (data == NULL || len == 0U || len > HPI_ESPFW_MAX_CHUNK || next == NULL) {
		return -EINVAL;
	}
	if (get_state() != HPI_ESPFW_RECEIVING) {
		return -ENOTSUP;
	}

	k_mutex_lock(&g_mx, K_FOREVER);
	if (get_state() != HPI_ESPFW_RECEIVING) {
		k_mutex_unlock(&g_mx);
		return -ENOTSUP;
	}
	s.last_chunk_at = k_uptime_get();

	/* The host resending the chunk accepted last (its reply was lost):
	 * already relayed and hashed -- acknowledge, do nothing else. */
	if (s.last_len != 0U && offset == s.last_off && len == s.last_len &&
	    offset + len == s.next) {
		*next = s.next;
		k_mutex_unlock(&g_mx);
		return 0;
	}
	if (offset != s.next || (uint64_t)offset + len > s.size) {
		LOG_WRN("ESP32 update: chunk +%u/%zu refused (expect +%u of %u)",
			offset, len, s.next, s.size);
		k_mutex_unlock(&g_mx);
		return -EINVAL;
	}

	rc = healthybridge_ota_request(hb, HPI_HB_OTA_OP_DATA, NULL, offset, data,
				       (uint16_t)len, HPI_ESPFW_REPLY_MS, &r);
	if (rc == -ETIMEDOUT) {
		/* Nothing hashed, nothing advanced: the host resends the same
		 * chunk, and the C6 acks a duplicate of the last one it took. */
		k_mutex_unlock(&g_mx);
		return -ETIMEDOUT;
	}
	if (rc != 0) {
		k_mutex_unlock(&g_mx);
		finish(HPI_ESPFW_FAILED, HPI_ESPFW_ERR_LINK, false);
		return -EHOSTDOWN;
	}

	uint8_t err = HPI_ESPFW_ERR_NONE;

	if (r.status != HPI_HB_OTA_OK) {
		note_c6_err(r.status);
		err = HPI_ESPFW_ERR_C6_REFUSED;
	} else if (r.u.ack.next_offset != offset + len || r.u.ack.image_size != s.size) {
		LOG_ERR("ESP32 update: C6 acked +%u with next %u size %u (expect %u, %u)",
			offset, r.u.ack.next_offset, r.u.ack.image_size,
			(uint32_t)(offset + len), s.size);
		err = HPI_ESPFW_ERR_PROTOCOL;
	} else if (psa_hash_update(&s.hash, data, len) != PSA_SUCCESS) {
		err = HPI_ESPFW_ERR_CRYPTO;
	}
	if (err != HPI_ESPFW_ERR_NONE) {
		c6_abort();
		k_mutex_unlock(&g_mx);
		finish(HPI_ESPFW_FAILED, err, false);
		return -EIO;
	}

	s.next = offset + (uint32_t)len;
	s.last_off = offset;
	s.last_len = (uint16_t)len;
	set_relayed(s.next);
	*next = s.next;
	k_mutex_unlock(&g_mx);
	return 0;
}

int hpi_esp32_update_commit(void)
{
	uint8_t got[HPI_ESPFW_SHA256_LEN];
	size_t olen = 0;
	uint8_t err = HPI_ESPFW_ERR_NONE;
	int ret = 0;

	if (get_state() != HPI_ESPFW_RECEIVING) {
		return -ENOTSUP;
	}
	k_mutex_lock(&g_mx, K_FOREVER);
	if (get_state() != HPI_ESPFW_RECEIVING) {
		k_mutex_unlock(&g_mx);
		return -ENOTSUP;
	}

	/* (a) every byte relayed and acknowledged */
	if (s.next != s.size) {
		LOG_ERR("ESP32 update: commit with %u of %u B relayed", s.next, s.size);
		err = HPI_ESPFW_ERR_INCOMPLETE;
		ret = -EINVAL;
	} else {
		psa_status_t ps = psa_hash_finish(&s.hash, got, sizeof(got), &olen);

		s.hashing = false;   /* finish ends the operation either way */
		if (ps != PSA_SUCCESS || olen != sizeof(got)) {
			err = HPI_ESPFW_ERR_CRYPTO;
			ret = -EIO;
		} else if (memcmp(got, s.sha, sizeof(got)) != 0) {
			/* (b) the bytes sent are the bytes whose digest was signed */
			LOG_ERR("ESP32 update: digest of the relayed image does not "
				"match the signed digest -- not committing");
			err = HPI_ESPFW_ERR_DIGEST;
			ret = -EBADMSG;
		}
	}

	if (err != HPI_ESPFW_ERR_NONE) {
		c6_abort();
		k_mutex_unlock(&g_mx);
		finish(HPI_ESPFW_FAILED, err, false);
		return ret;
	}

	/* (c) was verified at begin() over this same digest. */
	set_state(HPI_ESPFW_COMMITTING, HPI_ESPFW_ERR_NONE);
	k_mutex_unlock(&g_mx);
	k_sem_give(&g_kick);
	return 0;
}

int hpi_esp32_update_abort(void)
{
	switch (get_state()) {
	case HPI_ESPFW_PREPARING:
		/* The thread owns the link right now; it sees the flag between
		 * steps and sends ABORT if BEGIN has gone out. */
		atomic_set(&g_abort_req, 1);
		k_sem_give(&g_kick);
		return 0;
	case HPI_ESPFW_RECEIVING:
		k_mutex_lock(&g_mx, K_FOREVER);
		if (get_state() == HPI_ESPFW_RECEIVING) {
			c6_abort();
			k_mutex_unlock(&g_mx);
			finish(HPI_ESPFW_IDLE, HPI_ESPFW_ERR_ABORTED, false);
			return 0;
		}
		k_mutex_unlock(&g_mx);
		return hpi_esp32_update_abort();   /* moved on meanwhile */
	case HPI_ESPFW_COMMITTING:
	case HPI_ESPFW_REBOOTING:
	case HPI_ESPFW_VERIFYING:
		return -EBUSY;
	default:
		/* Nothing in flight: clear a finished session's result. */
		set_state(HPI_ESPFW_IDLE, HPI_ESPFW_ERR_NONE);
		return 0;
	}
}

bool hpi_esp32_update_busy(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lk);
	bool busy = session_active(g_st.state) ||
		    g_st.img == HPI_HB_OTA_IMG_PENDING_VERIFY;

	k_spin_unlock(&g_lk, key);
	return busy;
}

void hpi_esp32_update_status(struct hpi_espfw_status *out)
{
	if (out == NULL) {
		return;
	}
	k_spinlock_key_t key = k_spin_lock(&g_lk);

	*out = g_st;
	k_spin_unlock(&g_lk, key);
	out->busy = session_active(out->state) ||
		    out->img == HPI_HB_OTA_IMG_PENDING_VERIFY;
	if (out->version[0] == '\0') {
		/* Not asked over OTA STATUS (yet): the connectivity service's
		 * GET_VERSION cache, if it has one. */
		(void)hpi_connectivity_esp_version(out->version, sizeof(out->version));
	}
}

#else /* !CONFIG_HPI_ESP32_UPDATE or no co-processor link */

void hpi_esp32_update_init(void) { }

int hpi_esp32_update_begin(uint32_t size, const uint8_t sha256[HPI_ESPFW_SHA256_LEN],
			   const uint8_t *sig, size_t sig_len)
{
	ARG_UNUSED(size); ARG_UNUSED(sha256); ARG_UNUSED(sig); ARG_UNUSED(sig_len);
	return -ENOTSUP;
}

int hpi_esp32_update_chunk(uint32_t offset, const uint8_t *data, size_t len,
			   uint32_t *next)
{
	ARG_UNUSED(offset); ARG_UNUSED(data); ARG_UNUSED(len); ARG_UNUSED(next);
	return -ENOTSUP;
}

int hpi_esp32_update_commit(void) { return -ENOTSUP; }
int hpi_esp32_update_abort(void) { return -ENOTSUP; }
bool hpi_esp32_update_busy(void) { return false; }

void hpi_esp32_update_status(struct hpi_espfw_status *out)
{
	if (out != NULL) {
		memset(out, 0, sizeof(*out));
		out->c6_state = HPI_ESPFW_UNKNOWN;
		out->img = HPI_ESPFW_UNKNOWN;
		out->other = HPI_ESPFW_UNKNOWN;
		out->slot = HPI_ESPFW_UNKNOWN;
	}
}

#endif /* CONFIG_HPI_ESP32_UPDATE */
