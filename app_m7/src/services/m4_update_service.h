/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * M4 firmware update service (the M4 update path).
 *
 * The M4 cannot be an MCUboot image on this part: BOOT_CM4_ADD0 places the
 * reset vector at 64 KB granularity, so the header would have to be a 64 KB
 * multiple, but image_header.ih_hdr_size is uint16_t (max 65535). The M7 owns
 * the M4's update instead:
 *
 *   host --(group-64 chunks)--> m4_staging_partition (QSPI)
 *        --> verify digest
 *        --> erase + write bank 2 (0x0810_0000)
 *        --> reset, M4 restarts from the new image
 *
 * Bank 2 is only touched after the staged image is fully received and
 * verified; a failed or abandoned upload leaves it untouched, and the M4 keeps
 * auto-booting (BCM4=1) from whatever bank 2 holds.
 *
 * Single implementation of the capability; control/mcumgr_hpi is a thin
 * adapter over it (adapter-parity, see app_m7/ARCHITECTURE.md).
 */

#ifndef HPI_SERVICES_M4_UPDATE_SERVICE_H
#define HPI_SERVICES_M4_UPDATE_SERVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPI_M4FW_SHA256_LEN 32

/*
 * ECDSA-P256 signature over the image's SHA-256, as raw r||s (NOT DER — this
 * is what psa_verify_hash() takes). Host tools must convert (python:
 * cryptography...utils.decode_dss_signature, then two 32-byte big-endian
 * integers).
 */
#define HPI_M4FW_SIG_LEN 64

/* Upload state machine. Reported by hpi_m4_update_status(). */
enum hpi_m4fw_state {
	HPI_M4FW_IDLE = 0,      /* nothing in flight */
	HPI_M4FW_RECEIVING,     /* begin() accepted, chunks arriving */
	HPI_M4FW_VERIFIED,      /* digest matched; ready to commit */
	HPI_M4FW_COMMITTED,     /* bank 2 written; reset required */
	HPI_M4FW_FAILED,        /* see hpi_m4_update_status()->err */
};

struct hpi_m4fw_status {
	enum hpi_m4fw_state state;
	uint32_t total_size;    /* as declared by begin() */
	uint32_t received;      /* bytes written to staging so far */
	int      err;           /* last error (0 when none) */
};

/*
 * Start an upload. `size` is the whole image in bytes; `sha256` is its expected
 * digest, checked at commit time. Erases the staging region.
 *
 * `sig` is an ECDSA-P256 signature over that digest, made with the release
 * signing key (the same key MCUboot verifies the M7 against). Pass NULL with
 * sig_len 0 to omit it; whether that is accepted depends on
 * CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE. The device ships with the unlock
 * gate off, so without the signature anyone with USB access could put
 * arbitrary code on the M4.
 *
 * Returns -EBUSY if an upload is already in flight (call abort first),
 * -ENOSPC if the image does not fit staging or bank 2, -EINVAL for a malformed
 * signature, or a negative errno from the flash layer.
 */
int hpi_m4_update_begin(uint32_t size, const uint8_t sha256[HPI_M4FW_SHA256_LEN],
			const uint8_t *sig, size_t sig_len);

/* True when this build refuses unsigned M4 images. */
bool hpi_m4_update_signature_required(void);

/*
 * Write one chunk at `offset` into staging. Chunks must be sequential —
 * offset must equal the number of bytes received so far — so a dropped or
 * reordered chunk is rejected rather than silently leaving a hole.
 * Returns -EINVAL on a sequencing error, -ENOTSUP if no upload is in flight.
 */
int hpi_m4_update_chunk(uint32_t offset, const uint8_t *data, size_t len);

/*
 * Verify the staged image, then erase and rewrite bank 2 from it. On success the
 * M4's new image is in place but the M4 is still running the OLD one — the
 * caller must reset the system for it to take effect
 * (hpi_m4_update_reset_required() reports this).
 *
 * Verification, in order, all before bank 2 is touched:
 *   1. SHA-256 matches the digest from begin()          -> catches wire corruption
 *   2. ECDSA-P256 over that digest verifies              -> catches a forged image
 *   3. the image's first two vectors are plausible       -> catches the wrong file
 *
 * Step 3 matters: 1 and 2 both pass for a correctly-signed image that is not
 * an M4 image at all (an M7 build, say), which would leave the M4 dead until
 * an SWD reflash.
 *
 * Returns -EBADMSG on a digest or signature mismatch and -EILSEQ on an
 * implausible vector table; bank 2 is left untouched in every failure case.
 */
int hpi_m4_update_commit(void);

/* Discard any in-flight upload. Always safe; bank 2 is never touched. */
void hpi_m4_update_abort(void);

void hpi_m4_update_status(struct hpi_m4fw_status *out);

/* True once commit() has written bank 2 and a reset is needed for the M4 to run
 * the new image. */
bool hpi_m4_update_reset_required(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_M4_UPDATE_SERVICE_H */
