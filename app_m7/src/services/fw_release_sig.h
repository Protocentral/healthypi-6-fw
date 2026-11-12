/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Release-signature verifier -- the one place the M7 application checks that an
 * image was signed with the release key.
 *
 * Two images are verified by the application rather than by a bootloader: the
 * M4's (it cannot be an MCUboot image, see m4_update_service.h) and the
 * ESP32-C6's (relayed to the C6's own OTA slot, see esp32_update_service.h).
 * Both use the same scheme and the same key as MCUboot uses for the M7 image:
 * ECDSA-P256 over the image's SHA-256, raw r||s, verified against the public
 * half of the release key that tools/build/gen_m4_pubkey.py compiles into the
 * build. One key for the whole device, one verifier for the application.
 *
 * Compiled when CONFIG_HPI_FW_RELEASE_SIG is set (selected by the features that
 * need it); otherwise the call reports -ENOTSUP so a caller can never mistake
 * "not built" for "verified".
 */

#ifndef HPI_SERVICES_FW_RELEASE_SIG_H
#define HPI_SERVICES_FW_RELEASE_SIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPI_FW_SHA256_LEN 32

/*
 * ECDSA-P256 signature as raw r||s (NOT DER -- this is what psa_verify_hash()
 * takes). Host tools convert with
 * cryptography...utils.decode_dss_signature, then two 32-byte big-endian ints.
 */
#define HPI_FW_SIG_LEN 64

/*
 * Verify @p sig over @p digest with the release key.
 *
 * Returns 0 when it verifies, -EBADMSG when it does not, -EIO when the crypto
 * library failed (key import, init), -ENOTSUP when the verifier is not built.
 */
int hpi_fw_release_sig_verify(const uint8_t digest[HPI_FW_SHA256_LEN],
			      const uint8_t sig[HPI_FW_SIG_LEN]);

#ifdef __cplusplus
}
#endif

#endif /* HPI_SERVICES_FW_RELEASE_SIG_H */
