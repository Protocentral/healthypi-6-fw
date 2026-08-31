/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Device lock/unlock state machine -- see hpi_security.h. HMAC-SHA256 + CSPRNG via the PSA Crypto API (mbedTLS backend).
 */

#include "hpi_security.h"
#include "../mcumgr_hpi/hpi_mgmt_group.h"   /* HPI_MGMT_ERR_LOCK_* */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>   /* MGMT_ERR_* */
#include <string.h>

LOG_MODULE_REGISTER(hpi_sec, CONFIG_HPI_APP_LOG_LEVEL);

#if IS_ENABLED(CONFIG_HPI_SECURITY)

#include <psa/crypto.h>

#define NONCE_TTL_MS  60000U
#define GRANT_TTL_MS  (CONFIG_HPI_UNLOCK_GRANT_S * 1000U)

static uint8_t  secret[32];
static bool     secret_ok;

static uint8_t  nonce[HPI_UNLOCK_NONCE_LEN];
static bool     nonce_valid;
static int64_t  nonce_expiry_ms;

static enum hpi_lock_state state = HPI_LOCK_LOCKED;
static int64_t  grant_expiry_ms;

static struct k_spinlock lock;

/* -------- secret provisioning -------- */

static int hex2bin(const char *hex, uint8_t *out, size_t out_len)
{
	if (strlen(hex) != out_len * 2U) {
		return -EINVAL;
	}
	for (size_t i = 0; i < out_len; i++) {
		char c[3] = { hex[2 * i], hex[2 * i + 1], 0 };
		char *end = NULL;
		long v = strtol(c, &end, 16);
		if (end != c + 2) {
			return -EINVAL;
		}
		out[i] = (uint8_t)v;
	}
	return 0;
}

static void load_secret(void)
{
	secret_ok = false;
#if IS_ENABLED(CONFIG_HPI_UNLOCK_SECRET_SOURCE_KCONFIG)
	if (hex2bin(CONFIG_HPI_UNLOCK_SECRET_HEX, secret, sizeof(secret)) == 0) {
		secret_ok = true;
		LOG_WRN("dev-mode unlock secret from Kconfig -- DO NOT SHIP "
			"(provision the device-unique secret in OTP, §14.4)");
	} else {
		LOG_ERR("CONFIG_HPI_UNLOCK_SECRET_HEX must be 64 hex chars (32 bytes)");
	}
#else /* OTP source */
	/* TODO(hw §14.4): read the 256-bit device-unique secret from the STM32H7
	 * OTP / protected flash region. Until provisioning exists this stays
	 * unprovisioned and unlock is refused (fail-closed). */
	LOG_ERR("unlock secret source = OTP, but OTP provisioning not implemented "
		"yet -- device cannot be unlocked (fail-closed)");
#endif
}

void hpi_security_init(void)
{
	psa_status_t ps = psa_crypto_init();
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d); unlock unavailable", (int)ps);
	}
	load_secret();
	state = HPI_LOCK_LOCKED;
	nonce_valid = false;
	LOG_INF("device security: boot LOCKED (grant TTL %u s)", CONFIG_HPI_UNLOCK_GRANT_S);
}

/* -------- state -------- */

static bool unlocked_now_locked(void)   /* call under lock */
{
	if (state == HPI_LOCK_UNLOCKED && k_uptime_get() >= grant_expiry_ms) {
		state = HPI_LOCK_LOCKED;
	}
	return state == HPI_LOCK_UNLOCKED;
}

enum hpi_lock_state hpi_security_state(void)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	bool u = unlocked_now_locked();
	k_spin_unlock(&lock, k);
	return u ? HPI_LOCK_UNLOCKED : HPI_LOCK_LOCKED;
}

bool hpi_security_is_unlocked(void)
{
	return hpi_security_state() == HPI_LOCK_UNLOCKED;
}

void hpi_security_lock(void)
{
	k_spinlock_key_t k = k_spin_lock(&lock);
	state = HPI_LOCK_LOCKED;
	nonce_valid = false;
	k_spin_unlock(&lock, k);
	LOG_INF("device locked");
}

int hpi_security_require_unlocked(void)
{
	return hpi_security_is_unlocked() ? MGMT_ERR_EOK : MGMT_ERR_EACCES;
}

/* -------- challenge / response -------- */

int hpi_security_unlock_challenge(uint8_t *nonce_out, uint32_t *ttl_ms)
{
	if (!secret_ok) {
		return -ENOTSUP;
	}
	uint8_t fresh[HPI_UNLOCK_NONCE_LEN];
	if (psa_generate_random(fresh, sizeof(fresh)) != PSA_SUCCESS) {
		return -EIO;
	}
	k_spinlock_key_t k = k_spin_lock(&lock);
	memcpy(nonce, fresh, sizeof(nonce));     /* new challenge invalidates the old */
	nonce_valid = true;
	nonce_expiry_ms = k_uptime_get() + NONCE_TTL_MS;
	k_spin_unlock(&lock, k);

	memcpy(nonce_out, fresh, sizeof(fresh));
	if (ttl_ms) {
		*ttl_ms = NONCE_TTL_MS;
	}
	return 0;
}

int hpi_security_unlock_response(const uint8_t *tag, size_t tag_len, uint32_t *grant_ms)
{
	if (!secret_ok) {
		return -HPI_MGMT_ERR_LOCK_HMAC_INVALID;
	}
	if (tag_len != HPI_UNLOCK_TAG_LEN) {
		return -HPI_MGMT_ERR_LOCK_HMAC_INVALID;
	}

	/* Snapshot the outstanding nonce under lock; verify HMAC outside it. */
	uint8_t local_nonce[HPI_UNLOCK_NONCE_LEN];
	k_spinlock_key_t k = k_spin_lock(&lock);
	bool have = nonce_valid;
	bool expired = have && (k_uptime_get() >= nonce_expiry_ms);
	if (have) {
		memcpy(local_nonce, nonce, sizeof(local_nonce));
	}
	k_spin_unlock(&lock, k);

	if (!have) {
		return -HPI_MGMT_ERR_LOCK_CHALLENGE_MISSING;
	}
	if (expired) {
		return -HPI_MGMT_ERR_LOCK_EXPIRED;
	}

	/* PSA HMAC-SHA256 verify: import secret as an HMAC key, verify tag. */
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
	psa_set_key_bits(&attr, sizeof(secret) * 8U);

	psa_key_id_t key = PSA_KEY_ID_NULL;
	psa_status_t ps = psa_import_key(&attr, secret, sizeof(secret), &key);
	if (ps != PSA_SUCCESS) {
		return -HPI_MGMT_ERR_LOCK_HMAC_INVALID;
	}
	ps = psa_mac_verify(key, PSA_ALG_HMAC(PSA_ALG_SHA_256),
			    local_nonce, sizeof(local_nonce), tag, tag_len);
	psa_destroy_key(key);

	if (ps != PSA_SUCCESS) {
		LOG_WRN("unlock_response: HMAC mismatch");
		return -HPI_MGMT_ERR_LOCK_HMAC_INVALID;
	}

	k = k_spin_lock(&lock);
	state = HPI_LOCK_UNLOCKED;
	grant_expiry_ms = k_uptime_get() + GRANT_TTL_MS;
	nonce_valid = false;                      /* single-use */
	memset(nonce, 0, sizeof(nonce));          /* zeroize */
	k_spin_unlock(&lock, k);

	if (grant_ms) {
		*grant_ms = GRANT_TTL_MS;
	}
	LOG_INF("device unlocked (grant %u s)", CONFIG_HPI_UNLOCK_GRANT_S);
	return 0;
}

#else /* !CONFIG_HPI_SECURITY -- pre-security behavior: everything allowed */

void hpi_security_init(void) { }
enum hpi_lock_state hpi_security_state(void) { return HPI_LOCK_UNLOCKED; }
bool hpi_security_is_unlocked(void) { return true; }
void hpi_security_lock(void) { }
int hpi_security_require_unlocked(void) { return 0; /* MGMT_ERR_EOK */ }

int hpi_security_unlock_challenge(uint8_t *nonce_out, uint32_t *ttl_ms)
{
	ARG_UNUSED(nonce_out); ARG_UNUSED(ttl_ms);
	return -ENOTSUP;
}
int hpi_security_unlock_response(const uint8_t *tag, size_t tag_len, uint32_t *grant_ms)
{
	ARG_UNUSED(tag); ARG_UNUSED(tag_len); ARG_UNUSED(grant_ms);
	return -ENOTSUP;
}

#endif /* CONFIG_HPI_SECURITY */
