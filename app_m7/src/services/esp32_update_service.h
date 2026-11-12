/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * ESP32-C6 firmware update service (L4) -- relays a C6 application image from
 * the host into the C6's inactive OTA slot over the HealthyBridge link.
 *
 *   host --(group-64 espfw_chunk)--> M7 --(HealthyBridge 0x80 DATA)--> C6 slot
 *
 * Nothing is staged on the M7: each chunk the host sends is relayed to the C6,
 * acknowledged, and only then fed into a running SHA-256. The wire contract is
 * docs/PROTOCOL.md "Firmware update (0x80 / 0x81)" in healthybridge-esp32.
 *
 * Trust: the C6 does not check signatures. This service does, with the same
 * release key and verifier as the M4 image (services/fw_release_sig.h), and the
 * C6 never receives COMMIT unless all three hold:
 *   (a) exactly `size` bytes were relayed and acknowledged,
 *   (b) their SHA-256 equals the digest announced at begin(), and
 *   (c) the ECDSA-P256 signature over that digest verifies.
 * (c) is checked first, at begin(), over the announced digest -- a bad
 * signature costs no transfer -- and (b) at commit(), which is what ties the
 * verified digest to the bytes actually sent. Any failure sends ABORT.
 *
 * After COMMIT the C6 restarts into the new image PENDING_VERIFY and keeps it
 * only after 3 valid host frames within 60 s; any reset before then rolls it
 * back. So the service keeps the C6 powered and talking (its 1 Hz STATUS polls
 * are those frames) until the image reports VALID or the old image is back.
 *
 * Single implementation of the capability; control/mcumgr_hpi/hpi_espfw.c is a
 * thin adapter (adapter-parity, see app_m7/ARCHITECTURE.md). Gated by
 * CONFIG_HPI_ESP32_UPDATE; self-stubs (-ENOTSUP) otherwise.
 */

#ifndef HPI_SERVICES_ESP32_UPDATE_SERVICE_H
#define HPI_SERVICES_ESP32_UPDATE_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPI_ESPFW_SHA256_LEN 32
#define HPI_ESPFW_SIG_LEN    64     /* ECDSA-P256 raw r||s, as for the M4 */

/* Largest image begin() accepts before asking the C6. The C6's own limit is
 * its inactive slot (2,031,616 B on HealthyPi 6), which it enforces at BEGIN;
 * this only rejects absurd sizes without powering anything. */
#define HPI_ESPFW_MAX_IMAGE  (4U * 1024U * 1024U)

/* Most bytes one chunk() carries: one HealthyBridge DATA frame. In practice
 * the SMP packet size (512 B) limits a group-64 chunk well below this. */
#define HPI_ESPFW_MAX_CHUNK  1016U

/*
 * Bound on one chunk() call, i.e. on the espfw_chunk handler: the DATA frame's
 * transmit deadline (600 ms, reached only if the C6 holds RTS) plus the reply
 * wait (1000 ms). A host's per-request timeout must exceed it; 5 s is the
 * documented requirement (docs/MCUMGR_COMMANDS.md §5.12).
 */
#define HPI_ESPFW_REPLY_MS        1000U
#define HPI_ESPFW_CHUNK_BOUND_MS  1600U

/* Session state, reported by hpi_esp32_update_status() as `st`. */
enum hpi_espfw_state {
	HPI_ESPFW_IDLE = 0,        /* nothing in flight */
	HPI_ESPFW_PREPARING,       /* begin() accepted; powering the C6, sending BEGIN */
	HPI_ESPFW_RECEIVING,       /* the C6 accepted BEGIN; send chunks */
	HPI_ESPFW_COMMITTING,      /* (a)(b)(c) hold; COMMIT sent, C6 verifying */
	HPI_ESPFW_REBOOTING,       /* COMMIT accepted; waiting for the C6 to restart */
	HPI_ESPFW_VERIFYING,       /* new image running PENDING_VERIFY; proving the link */
	HPI_ESPFW_DONE,            /* success: new image running, VALID */
	HPI_ESPFW_ROLLED_BACK,     /* new image failed its self-test; old image running */
	HPI_ESPFW_FAILED,          /* see err / c6_err; the C6's boot slot is unchanged */
	HPI_ESPFW_TIMEOUT,         /* outcome not confirmed in time; C6 left powered */
};

/* Why the last session ended badly, reported as `err`. 0 = no error. */
enum hpi_espfw_err {
	HPI_ESPFW_ERR_NONE = 0,
	HPI_ESPFW_ERR_SIG_INVALID,  /* 1  signature does not verify; nothing was sent */
	HPI_ESPFW_ERR_LINK,         /* 2  the C6 could not be powered, or never answered */
	HPI_ESPFW_ERR_NO_OTA,       /* 3  the C6 answers, but not OTA STATUS: its
				     *    firmware predates the 0x80/0x81 update path */
	HPI_ESPFW_ERR_C6_REFUSED,   /* 4  the C6 refused a request; raw code in c6_err */
	HPI_ESPFW_ERR_NO_REPLY,     /* 5  the C6 stopped answering mid-session */
	HPI_ESPFW_ERR_PROTOCOL,     /* 6  a C6 reply disagrees with the transfer */
	HPI_ESPFW_ERR_DIGEST,       /* 7  SHA-256 of the relayed bytes != announced */
	HPI_ESPFW_ERR_INCOMPLETE,   /* 8  commit before every byte was relayed */
	HPI_ESPFW_ERR_ABORTED,      /* 9  the host aborted */
	HPI_ESPFW_ERR_HOST_IDLE,    /* 10 no chunk from the host for 30 s */
	HPI_ESPFW_ERR_CRYPTO,       /* 11 the crypto library failed */
	HPI_ESPFW_ERR_NOT_BACK,     /* 12 the C6 did not come back after COMMIT */
	HPI_ESPFW_ERR_NOT_VALID,    /* 13 it came back but never reported VALID */
	HPI_ESPFW_ERR_ROLLED_BACK,  /* 14 the new image rolled itself back */
	HPI_ESPFW_ERR_NOT_SWITCHED, /* 15 the C6 restarted on its old image without
				     *    recording a rollback: the switch never ran */
};

/* 0xFF in any C6 field below = not known (never asked, or no answer). */
#define HPI_ESPFW_UNKNOWN 0xFFU

struct hpi_espfw_status {
	uint8_t  state;        /* enum hpi_espfw_state */
	uint8_t  err;          /* enum hpi_espfw_err */
	uint8_t  c6_err;       /* raw HealthyBridge OTA status code (HPI_HB_OTA_*) of the
				* last refusal, or the C6's own last_error */
	uint8_t  c6_state;     /* C6 receiver state HPI_HB_OTA_STATE_* */
	uint8_t  img;          /* running image state HPI_HB_OTA_IMG_* */
	uint8_t  other;        /* other slot's image state */
	uint8_t  slot;         /* running slot: 0 / 1 */
	bool     busy;         /* hpi_esp32_update_busy() */
	uint32_t size;         /* image size announced at begin() */
	uint32_t relayed;      /* bytes relayed and acknowledged by the C6 */
	char     version[32];  /* C6 running version, "" if unknown */
};

/* Start the service thread. No-op when CONFIG_HPI_ESP32_UPDATE=n. */
void hpi_esp32_update_init(void);

/*
 * Start a session. Verifies @p sig (ECDSA-P256, raw r||s, over @p sha256) with
 * the release key BEFORE anything else -- a bad signature costs no transfer and
 * never wakes the C6. Then returns at once; the service thread powers the C6 if
 * needed, takes over the link and sends BEGIN. Poll status until `state` is
 * RECEIVING (or FAILED).
 *
 * Returns 0, -EINVAL (size 0, missing digest, signature not 64 bytes),
 * -ENOSPC (size over HPI_ESPFW_MAX_IMAGE), -EBADMSG (signature does not
 * verify), -EBUSY (a session is in progress, or the C6's running image is
 * still pending verification), -ENODEV (no co-processor link), -EIO (crypto).
 */
int hpi_esp32_update_begin(uint32_t size, const uint8_t sha256[HPI_ESPFW_SHA256_LEN],
			   const uint8_t *sig, size_t sig_len);

/*
 * Relay one chunk and wait for the C6's acknowledgement (bounded by
 * HPI_ESPFW_CHUNK_BOUND_MS). Chunks are strictly sequential: @p offset must be
 * the next expected offset (returned in @p next). Resending the chunk that was
 * accepted last -- same offset and length -- is acknowledged without being
 * relayed or hashed again, so a host that lost a reply may simply resend.
 *
 * Returns 0 with *next updated; -ETIMEDOUT when the C6 did not answer (the
 * session continues: resend the same chunk); -ENOTSUP when no session is
 * receiving; -EINVAL out of order, empty, too long or past the size; -EIO when
 * the C6 refused or answered inconsistently (the session has FAILED and ABORT
 * was sent); -EHOSTDOWN when the link went down (FAILED).
 */
int hpi_esp32_update_chunk(uint32_t offset, const uint8_t *data, size_t len,
			   uint32_t *next);

/*
 * Check (a) and (b) -- (c) was checked at begin() -- and hand COMMIT to the
 * service thread. Returns promptly; poll status for the outcome, which takes
 * ~1 s of C6 verification, a restart, and up to 3 s of link self-test (allow
 * ~95 s in all before treating TIMEOUT as final: the device waits up to 75 s
 * for VALID, past the C6's own 60 s self-test window).
 *
 * Returns 0; -ENOTSUP when not receiving; -EINVAL when fewer than `size` bytes
 * were relayed and -EBADMSG when their digest differs (both send ABORT and end
 * the session); -EIO on a crypto failure (also aborts).
 */
int hpi_esp32_update_commit(void);

/*
 * End a session that has not committed: sends ABORT to the C6 (whose running
 * image and boot slot are untouched) and returns to IDLE with err ABORTED.
 * Clears a finished session's result. -EBUSY once COMMIT has been sent -- the
 * outcome is the C6's now, and stopping the watch would risk a reset of a
 * pending image.
 */
int hpi_esp32_update_abort(void);

/* Snapshot; never blocks on the link. */
void hpi_esp32_update_status(struct hpi_espfw_status *out);

/*
 * True while resetting the C6 -- or the M7, which resets the C6 -- would lose
 * an update: a session between begin() and a final result, or a C6 whose
 * running image is known to be PENDING_VERIFY. The guard behind conn_disable,
 * enter_recovery and the MCUmgr OS reset.
 */
bool hpi_esp32_update_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_ESP32_UPDATE_SERVICE_H */
