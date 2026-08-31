/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Device lock / unlock state machine.
 *
 * The device boots LOCKED. Read-only, non-PHI commands work while locked;
 * destructive / PHI / privileged commands call hpi_security_require_unlocked()
 * and get MGMT_ERR_EACCES until the host completes a challenge-response unlock:
 *   1. hpi/unlock_challenge -> device returns a 16-byte CSPRNG nonce (60 s TTL)
 *   2. host computes HMAC-SHA256(shared_secret, nonce) = tag[32]
 *   3. hpi/unlock_response{tag} -> device verifies; on success UNLOCKED for the
 *      grant TTL (default 10 min), then auto-relocks.
 *
 * Gated behind CONFIG_HPI_SECURITY. When disabled, require_unlocked() is a
 * no-op (everything allowed) and is_unlocked() is always true. Enable it
 * together with the signing/OTP work (§14.5): a real deployment needs the
 * device-unique secret in OTP.
 */

#ifndef HPI_SECURITY_H
#define HPI_SECURITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPI_UNLOCK_NONCE_LEN   16
#define HPI_UNLOCK_TAG_LEN     32   /* HMAC-SHA256 */

enum hpi_lock_state {
	HPI_LOCK_LOCKED   = 0,
	HPI_LOCK_UNLOCKED = 1,
};

/* Load the shared secret + reset to LOCKED. Call once at boot. */
void hpi_security_init(void);

/* Effective lock state (auto-relocks when the grant TTL has elapsed). */
enum hpi_lock_state hpi_security_state(void);
bool hpi_security_is_unlocked(void);

/* Force LOCKED (host hpi/lock, CDC1 DTR-down, etc.). */
void hpi_security_lock(void);

/* Generate a fresh single-use nonce (invalidates any prior one). Fills
 * nonce_out[HPI_UNLOCK_NONCE_LEN]; *ttl_ms = nonce validity window. 0 / errno. */
int hpi_security_unlock_challenge(uint8_t *nonce_out, uint32_t *ttl_ms);

/* Verify HMAC tag over the outstanding nonce. On success: UNLOCKED, *grant_ms =
 * grant TTL. Returns 0, or a negative HPI_MGMT_ERR_LOCK_* error code value. */
int hpi_security_unlock_response(const uint8_t *tag, size_t tag_len,
				 uint32_t *grant_ms);

/* Gate for privileged MCUmgr handlers. Returns MGMT_ERR_EOK (0) if allowed, or
 * MGMT_ERR_EACCES if the device is locked. Always 0 when CONFIG_HPI_SECURITY=n. */
int hpi_security_require_unlocked(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SECURITY_H */
