/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * M4 firmware update service -- see m4_update_service.h for the design and why
 * the M4 is not an MCUboot image.
 */

#include "m4_update_service.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>

/* The staging region lives in the MCUboot partition map, which only the signed
 * flavor applies -- the dev build's QSPI has xip at 0x0 with no free region. So
 * this capability is signed-flavor only and self-stubs elsewhere, keeping the
 * MCUmgr adapter and main() link-clean either way. */
#define M4FW_STAGING_NODE DT_NODELABEL(m4_staging_partition)

#if IS_ENABLED(CONFIG_HPI_M4_UPDATE) && DT_NODE_EXISTS(M4FW_STAGING_NODE)
#define M4FW_AVAILABLE 1

#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <psa/crypto.h>

LOG_MODULE_REGISTER(hpi_m4fw, CONFIG_HPI_APP_LOG_LEVEL);

#define M4FW_STAGING_ID FIXED_PARTITION_ID(m4_staging_partition)

/*
 * Bank 2 -- the M4's execution region -- addressed through the M7's flash0.
 * Deliberately NOT a DT partition (it is not an MCUboot slot). flash0's reg
 * covers bank 1 only, but bank2-flash-size = <1024> on the board DTS makes the
 * driver treat the part as 2 MB contiguous, so bank-2 offsets are valid. See
 * the slot map dtsi.
 */
#define M4FW_BANK2_OFF   0x100000U
#define M4FW_BANK2_SIZE  (1024U * 1024U)
#define M4FW_ERASE_BLOCK (128U * 1024U)   /* bank 2 sector size */

/* Copy granularity for staging -> bank 2. Bank 2 writes need 32-byte alignment
 * (write-block-size); 4 KB keeps the stack allocation off and the loop cheap. */
#define M4FW_COPY_CHUNK 4096U

static const struct device *const bank2_dev =
	DEVICE_DT_GET(DT_MTD_FROM_FIXED_PARTITION(DT_NODELABEL(slot0_partition)));

/*
 * Plausibility bounds for the M4's vector table (image words 0 and 1).
 *
 * word 0 = initial stack pointer, word 1 = reset handler.
 *
 * The M4's stack lives in SRAM (AXI SRAM / D2 SRAM1-3 in this SoC's map, all
 * inside 0x24000000-0x30050000), and its reset handler must be in bank 2 where
 * BOOT_CM4_ADD0 points. Anything else is not an M4 image for this board.
 */
#define M4FW_SRAM_LO   0x20000000U   /* DTCM base -- lowest RAM the M4 can stack in */
#define M4FW_SRAM_HI   0x30050000U   /* end of SRAM3 (D2) */
#define M4FW_BANK2_LO  0x08100000U
#define M4FW_BANK2_HI  (M4FW_BANK2_LO + M4FW_BANK2_SIZE)

static struct {
	enum hpi_m4fw_state state;
	uint32_t total;
	uint32_t received;
	int      err;
	uint8_t  want_sha[HPI_M4FW_SHA256_LEN];
	uint8_t  sig[HPI_M4FW_SIG_LEN];
	bool     have_sig;
	struct k_mutex lock;
} m4fw = {
	.lock = Z_MUTEX_INITIALIZER(m4fw.lock),
};

#if IS_ENABLED(CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE)
/* Generated at build time from the release signing key by
 * tools/build/gen_m4_pubkey.py -- raw uncompressed EC point. */
extern const uint8_t hpi_m4fw_pubkey[65];
#endif

/* ---- helpers ---- */

static int staging_sha256(uint32_t len, uint8_t out[HPI_M4FW_SHA256_LEN])
{
	/* PSA must be initialised before any operation, and no other caller can
	 * be assumed to have done it (control/security may be compiled out).
	 * Without this every psa_hash_* call returns PSA_ERROR_BAD_STATE.
	 * psa_crypto_init() is idempotent, so calling it here is always safe. */
	psa_status_t ps = psa_crypto_init();
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d)", (int)ps);
		return -EIO;
	}

	const struct flash_area *fa;
	int rc = flash_area_open(M4FW_STAGING_ID, &fa);
	if (rc != 0) {
		LOG_ERR("staging open failed (%d)", rc);
		return rc;
	}

	psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
	ps = psa_hash_setup(&op, PSA_ALG_SHA_256);
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_hash_setup failed (%d)", (int)ps);
		flash_area_close(fa);
		return -EIO;
	}

	static uint8_t buf[M4FW_COPY_CHUNK];
	uint32_t off = 0;
	rc = 0;
	while (off < len) {
		uint32_t n = MIN((uint32_t)sizeof(buf), len - off);
		rc = flash_area_read(fa, off, buf, n);
		if (rc != 0) {
			break;
		}
		ps = psa_hash_update(&op, buf, n);
		if (ps != PSA_SUCCESS) {
			LOG_ERR("psa_hash_update failed (%d) at +%u", (int)ps, off);
			rc = -EIO;
			break;
		}
		off += n;
	}
	flash_area_close(fa);

	if (rc != 0) {
		psa_hash_abort(&op);
		LOG_ERR("staging read failed (%d)", rc);
		return rc;
	}

	size_t olen = 0;
	ps = psa_hash_finish(&op, out, HPI_M4FW_SHA256_LEN, &olen);
	if (ps != PSA_SUCCESS || olen != HPI_M4FW_SHA256_LEN) {
		LOG_ERR("psa_hash_finish failed (%d, olen=%zu)", (int)ps, olen);
		return -EIO;
	}
	return 0;
}

/* Verify the ECDSA-P256 signature over the image digest. Anchored to the same
 * key MCUboot verifies the M7 against -- one key for the whole device. */
static int verify_signature(const uint8_t digest[HPI_M4FW_SHA256_LEN])
{
#if IS_ENABLED(CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE)
	if (!m4fw.have_sig) {
		LOG_ERR("M4 update: no signature supplied and this build requires one");
		return -EBADMSG;
	}

	psa_status_t ps = psa_crypto_init();
	if (ps != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed (%d)", (int)ps);
		return -EIO;
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);

	psa_key_id_t key = PSA_KEY_ID_NULL;
	ps = psa_import_key(&attr, hpi_m4fw_pubkey, sizeof(hpi_m4fw_pubkey), &key);
	if (ps != PSA_SUCCESS) {
		LOG_ERR("M4 update: public key import failed (%d)", (int)ps);
		return -EIO;
	}

	/* verify_HASH, not verify_message: the digest is already computed and
	 * checked, so there is no need to re-hash the staged image. */
	ps = psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
			     digest, HPI_M4FW_SHA256_LEN,
			     m4fw.sig, HPI_M4FW_SIG_LEN);
	psa_destroy_key(key);

	if (ps != PSA_SUCCESS) {
		LOG_ERR("M4 update: signature INVALID (%d) -- bank 2 not touched", (int)ps);
		return -EBADMSG;
	}
	LOG_INF("M4 update: signature OK");
	return 0;
#else
	ARG_UNUSED(digest);
	if (m4fw.have_sig) {
		LOG_WRN("M4 update: signature supplied but this build does not verify it");
	}
	return 0;
#endif
}

/*
 * Reject an image that verifies perfectly but is not an M4 image: a signed M7
 * build uploaded by mistake passes both the digest and signature checks, and
 * writing it would leave the M4 dead until an SWD reflash. Two words of the
 * vector table are enough to catch it.
 */
static int verify_vectors(void)
{
	const struct flash_area *fa;
	int rc = flash_area_open(M4FW_STAGING_ID, &fa);
	if (rc != 0) {
		return rc;
	}

	uint32_t vec[2];
	rc = flash_area_read(fa, 0, vec, sizeof(vec));
	flash_area_close(fa);
	if (rc != 0) {
		return rc;
	}

	if (vec[0] < M4FW_SRAM_LO || vec[0] > M4FW_SRAM_HI) {
		LOG_ERR("M4 update: initial SP 0x%08x is not in SRAM -- not an M4 image",
			vec[0]);
		return -EILSEQ;
	}
	/* Thumb reset vectors carry bit 0 set; mask it before the range test. */
	uint32_t pc = vec[1] & ~1U;
	if (pc < M4FW_BANK2_LO || pc >= M4FW_BANK2_HI) {
		LOG_ERR("M4 update: reset vector 0x%08x is outside bank 2 -- not an M4 image",
			vec[1]);
		return -EILSEQ;
	}

	LOG_INF("M4 update: vectors plausible (SP=0x%08x PC=0x%08x)", vec[0], vec[1]);
	return 0;
}

/* Erase + rewrite bank 2 from the staged image. Only called after the digest has
 * matched, so a corrupt upload never reaches this. */
static int commit_to_bank2(uint32_t len)
{
	const struct flash_area *fa;
	int rc = flash_area_open(M4FW_STAGING_ID, &fa);
	if (rc != 0) {
		return rc;
	}

	/* Erase whole sectors covering the image. */
	uint32_t erase_len = ROUND_UP(len, M4FW_ERASE_BLOCK);
	LOG_INF("M4 update: erasing bank 2 (%u B)", erase_len);
	rc = flash_erase(bank2_dev, M4FW_BANK2_OFF, erase_len);
	if (rc != 0) {
		LOG_ERR("M4 update: bank 2 erase failed (%d)", rc);
		flash_area_close(fa);
		return rc;
	}

	static uint8_t buf[M4FW_COPY_CHUNK];
	uint32_t off = 0;
	while (off < len) {
		uint32_t n = MIN((uint32_t)sizeof(buf), len - off);
		/* Bank 2 needs 32-byte-aligned writes; pad the tail with 0xFF so the
		 * final partial chunk does not fail alignment. */
		uint32_t wn = ROUND_UP(n, 32U);
		if (wn > n) {
			memset(buf + n, 0xFF, wn - n);
		}
		rc = flash_area_read(fa, off, buf, n);
		if (rc != 0) {
			break;
		}
		rc = flash_write(bank2_dev, M4FW_BANK2_OFF + off, buf, wn);
		if (rc != 0) {
			LOG_ERR("M4 update: bank 2 write failed at +%u (%d)", off, rc);
			break;
		}
		off += n;
	}
	flash_area_close(fa);

	if (rc == 0) {
		LOG_INF("M4 update: bank 2 written (%u B) -- reset required", len);
	}
	return rc;
}

/* ---- public API ---- */

bool hpi_m4_update_signature_required(void)
{
	return IS_ENABLED(CONFIG_HPI_M4_UPDATE_REQUIRE_SIGNATURE);
}

int hpi_m4_update_begin(uint32_t size, const uint8_t sha256[HPI_M4FW_SHA256_LEN],
			const uint8_t *sig, size_t sig_len)
{
	if (size == 0U || sha256 == NULL) {
		return -EINVAL;
	}
	/* A wrong-length signature is a client bug, not an authentication
	 * failure -- say so now rather than at commit, after a whole upload. */
	if (sig != NULL && sig_len != HPI_M4FW_SIG_LEN) {
		LOG_WRN("M4 update: signature is %zu B, expected %d (raw r||s)",
			sig_len, HPI_M4FW_SIG_LEN);
		return -EINVAL;
	}
	if (sig == NULL && hpi_m4_update_signature_required()) {
		LOG_WRN("M4 update: this build requires a signed image");
		return -EBADMSG;
	}
	if (!device_is_ready(bank2_dev)) {
		return -ENODEV;
	}

	k_mutex_lock(&m4fw.lock, K_FOREVER);

	if (m4fw.state == HPI_M4FW_RECEIVING) {
		k_mutex_unlock(&m4fw.lock);
		return -EBUSY;
	}

	const struct flash_area *fa;
	int rc = flash_area_open(M4FW_STAGING_ID, &fa);
	if (rc != 0) {
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}
	if (size > fa->fa_size || size > M4FW_BANK2_SIZE) {
		LOG_WRN("M4 update: image %u B exceeds staging (%zu) or bank 2 (%u)",
			size, (size_t)fa->fa_size, M4FW_BANK2_SIZE);
		flash_area_close(fa);
		k_mutex_unlock(&m4fw.lock);
		return -ENOSPC;
	}

	rc = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);
	if (rc != 0) {
		LOG_ERR("M4 update: staging erase failed (%d)", rc);
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = rc;
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}

	m4fw.state = HPI_M4FW_RECEIVING;
	m4fw.total = size;
	m4fw.received = 0;
	m4fw.err = 0;
	memcpy(m4fw.want_sha, sha256, HPI_M4FW_SHA256_LEN);
	m4fw.have_sig = (sig != NULL);
	if (m4fw.have_sig) {
		memcpy(m4fw.sig, sig, HPI_M4FW_SIG_LEN);
	} else {
		memset(m4fw.sig, 0, sizeof(m4fw.sig));
	}

	LOG_INF("M4 update: begin, %u B (%s)", size,
		m4fw.have_sig ? "signed" : "unsigned");
	k_mutex_unlock(&m4fw.lock);
	return 0;
}

int hpi_m4_update_chunk(uint32_t offset, const uint8_t *data, size_t len)
{
	if (data == NULL || len == 0U) {
		return -EINVAL;
	}

	k_mutex_lock(&m4fw.lock, K_FOREVER);

	if (m4fw.state != HPI_M4FW_RECEIVING) {
		k_mutex_unlock(&m4fw.lock);
		return -ENOTSUP;
	}
	/* Sequential only: a reordered or dropped chunk must fail loudly rather
	 * than leave an unwritten hole that still passes the length check. */
	if (offset != m4fw.received) {
		LOG_WRN("M4 update: chunk out of order (got +%u, want +%u)",
			offset, m4fw.received);
		k_mutex_unlock(&m4fw.lock);
		return -EINVAL;
	}
	if (offset + len > m4fw.total) {
		k_mutex_unlock(&m4fw.lock);
		return -ENOSPC;
	}

	const struct flash_area *fa;
	int rc = flash_area_open(M4FW_STAGING_ID, &fa);
	if (rc == 0) {
		rc = flash_area_write(fa, offset, data, len);
		flash_area_close(fa);
	}
	if (rc != 0) {
		LOG_ERR("M4 update: staging write failed at +%u (%d)", offset, rc);
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = rc;
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}

	m4fw.received += len;
	k_mutex_unlock(&m4fw.lock);
	return 0;
}

int hpi_m4_update_commit(void)
{
	k_mutex_lock(&m4fw.lock, K_FOREVER);

	if (m4fw.state != HPI_M4FW_RECEIVING) {
		k_mutex_unlock(&m4fw.lock);
		return -ENOTSUP;
	}
	if (m4fw.received != m4fw.total) {
		LOG_WRN("M4 update: incomplete (%u/%u B)", m4fw.received, m4fw.total);
		k_mutex_unlock(&m4fw.lock);
		return -EINVAL;
	}

	uint8_t got[HPI_M4FW_SHA256_LEN];
	int rc = staging_sha256(m4fw.total, got);
	if (rc != 0) {
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = rc;
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}
	/* Bank 2 is untouched on mismatch -- the M4 keeps running its old image. */
	if (memcmp(got, m4fw.want_sha, HPI_M4FW_SHA256_LEN) != 0) {
		LOG_ERR("M4 update: digest mismatch -- bank 2 not touched");
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = -EBADMSG;
		k_mutex_unlock(&m4fw.lock);
		return -EBADMSG;
	}

	rc = verify_signature(got);
	if (rc == 0) {
		rc = verify_vectors();
	}
	if (rc != 0) {
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = rc;
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}

	m4fw.state = HPI_M4FW_VERIFIED;

	rc = commit_to_bank2(m4fw.total);
	if (rc != 0) {
		m4fw.state = HPI_M4FW_FAILED;
		m4fw.err = rc;
		k_mutex_unlock(&m4fw.lock);
		return rc;
	}

	m4fw.state = HPI_M4FW_COMMITTED;
	k_mutex_unlock(&m4fw.lock);
	return 0;
}

void hpi_m4_update_abort(void)
{
	k_mutex_lock(&m4fw.lock, K_FOREVER);
	if (m4fw.state == HPI_M4FW_RECEIVING || m4fw.state == HPI_M4FW_FAILED) {
		LOG_INF("M4 update: aborted at %u/%u B", m4fw.received, m4fw.total);
		m4fw.state = HPI_M4FW_IDLE;
		m4fw.received = 0;
		m4fw.total = 0;
		m4fw.err = 0;
	}
	k_mutex_unlock(&m4fw.lock);
}

void hpi_m4_update_status(struct hpi_m4fw_status *out)
{
	if (out == NULL) {
		return;
	}
	k_mutex_lock(&m4fw.lock, K_FOREVER);
	out->state = m4fw.state;
	out->total_size = m4fw.total;
	out->received = m4fw.received;
	out->err = m4fw.err;
	k_mutex_unlock(&m4fw.lock);
}

bool hpi_m4_update_reset_required(void)
{
	return m4fw.state == HPI_M4FW_COMMITTED;
}

#else /* !M4FW_AVAILABLE -- dev flavor has no staging region */

bool hpi_m4_update_signature_required(void)
{
	return false;
}

int hpi_m4_update_begin(uint32_t size, const uint8_t sha256[HPI_M4FW_SHA256_LEN],
			const uint8_t *sig, size_t sig_len)
{
	ARG_UNUSED(size); ARG_UNUSED(sha256); ARG_UNUSED(sig); ARG_UNUSED(sig_len);
	return -ENOTSUP;
}

int hpi_m4_update_chunk(uint32_t offset, const uint8_t *data, size_t len)
{
	ARG_UNUSED(offset); ARG_UNUSED(data); ARG_UNUSED(len);
	return -ENOTSUP;
}

int hpi_m4_update_commit(void)
{
	return -ENOTSUP;
}

void hpi_m4_update_abort(void) { }

void hpi_m4_update_status(struct hpi_m4fw_status *out)
{
	if (out != NULL) {
		*out = (struct hpi_m4fw_status){ .state = HPI_M4FW_IDLE };
	}
}

bool hpi_m4_update_reset_required(void)
{
	return false;
}

#endif /* M4FW_AVAILABLE */
