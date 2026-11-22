/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Release-signature verifier -- see fw_release_sig.h. Moved here unchanged from
 * m4_update_service.c so the ESP32-C6 update path verifies with the same code
 * rather than a second copy of it.
 */

#include "fw_release_sig.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <psa/crypto.h>

LOG_MODULE_REGISTER(hpi_fwsig, CONFIG_HPI_APP_LOG_LEVEL);

/* Generated at build time from the release key(s) by
 * tools/build/gen_m4_pubkey.py -- raw uncompressed EC points, the primary
 * first, then the backup when the build has one (CONFIG_HPI_BACKUP_SIGNING_KEY). */
extern const uint8_t hpi_fw_release_pubkeys[][65];
extern const unsigned int hpi_fw_release_pubkey_count;

/* 0 if `sig` verifies against key `idx`, -EBADMSG if it does not, -EIO if the
 * key could not be used at all. */
static int verify_with(unsigned int idx, const uint8_t digest[HPI_FW_SHA256_LEN],
		       const uint8_t sig[HPI_FW_SIG_LEN])
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t ps = psa_import_key(&attr, hpi_fw_release_pubkeys[idx], 65, &key);
	if (ps != PSA_SUCCESS) {
		LOG_ERR("release key %u import failed (%d)", idx, (int)ps);
		return -EIO;
	}

	/* verify_HASH, not verify_message: the caller holds the digest. */
	ps = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
			     digest, HPI_FW_SHA256_LEN, sig, HPI_FW_SIG_LEN);
	psa_destroy_key(key);
	return ps == PSA_SUCCESS ? 0 : -EBADMSG;
}

int hpi_fw_release_sig_verify(const uint8_t digest[HPI_FW_SHA256_LEN],
			      const uint8_t sig[HPI_FW_SIG_LEN])
{
	if (digest == NULL || sig == NULL) {
		return -EINVAL;
	}

	/* Idempotent; no other caller can be assumed to have done it. */
	psa_status_t ps = psa_crypto_init();
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d)", (int)ps);
		return -EIO;
	}

	/* Accept a signature from any trusted key. A key that cannot be imported
	 * is a build defect, so it is reported, but it does not stop the other
	 * key from being tried. */
	int rc = -EBADMSG;
	for (unsigned int i = 0; i < hpi_fw_release_pubkey_count; i++) {
		int r = verify_with(i, digest, sig);
		if (r == 0) {
			if (i > 0) {
				LOG_INF("release signature valid (backup key %u)", i);
			}
			return 0;
		}
		if (r == -EIO) {
			rc = -EIO;
		}
	}

	if (rc == -EBADMSG) {
		LOG_ERR("release signature INVALID against all %u key(s)",
			hpi_fw_release_pubkey_count);
	}
	return rc;
}
