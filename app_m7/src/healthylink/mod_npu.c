/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink Compute module provider (STM32N657 NPU, SPI slave).
 * Registers in the iterable provider section; the framework starts it when a
 * HealthyLink Compute module is detected.
 *
 * Bus: the NPU lives on **SPI4** (shared by both slots), NOT SPI6 -- touching
 * SPI6 wedges the next SPI4 transceive. SPI4 is `st,soft-nss` with NO cs-gpios,
 * so this file drives the controller directly and supplies the chip-select
 * itself (see npu_cs_lines).
 *
 * PROTOCOL: HLink v2 (hlink_proto.h). start() runs a handshake off the boot
 * path -- alive signature, GET_INFO, STATUS -- and caches the result for
 * hpi_npu_link_get(). npu_cmd() is the shared SPI helper (npu_link.h):
 * NPU_WAIT_REPLY for handshake / RUN, NPU_WAIT_ACK (2 ms) for STREAM_PUSH.
 * The data plane is not driven from this file.
 *
 * The host spoke v1 until 2026-09: a bare command byte at offset 0, no CRC, no
 * sequence number, a fixed 50 ms turnaround and an exact 8-byte match on the
 * alive signature -- INCLUDING its version byte, so a v2 module reported "no
 * HLNK signature, check your wiring". Nothing here falls back to v1: its
 * numbering collides with v2 (v1 LOAD_INPUT 0x10 is v2 MODEL_LIST, v1
 * RUN_INFERENCE 0x20 is TENSOR_LOAD, v1 READ_OUTPUT 0x30 is STREAM_PUSH), and
 * two live numberings in one file is how a plausible wrong answer is produced.
 * A v1 module is identified and reported. It is not driven.
 */

#include "hl_provider.h"
#include "hlink_proto.h"
#include "mod_npu.h"
#include "npu_link.h"
#include "npu_uart_host.h"             /* parked transport; see its header */
#if IS_ENABLED(CONFIG_HPI_NPU_STREAM)
#include "npu_stream.h"
#endif
#if IS_ENABLED(CONFIG_HPI_NPU_INFER)
#include "npu_infer.h"
#endif

#include <healthylink/healthylink.h>   /* module IDs + capability bits */
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(mod_npu, CONFIG_HPI_APP_LOG_LEVEL);

#define NPU_SPI_NODE DT_NODELABEL(spi4)

/*
 * The SPI4 handshake is OFF by default (CONFIG_HPI_NPU_COMMS_CHECK): a faulting
 * transceive resets the MCU (boot loop). The NPU is still detected, registered
 * and marked ACTIVE without it.
 */
#if DT_NODE_HAS_STATUS(NPU_SPI_NODE, okay) && IS_ENABLED(CONFIG_HPI_NPU_COMMS_CHECK)
#define NPU_SPI_AVAILABLE 1
#else
#define NPU_SPI_AVAILABLE 0
#endif

/* ---- the handshake snapshot (read by the UI and by group-64 selftest) ---- */

static struct hpi_npu_link_info g_link = {
	.link_state = NPU_SPI_AVAILABLE ? HPI_NPU_LINK_NOT_RUN
					: HPI_NPU_LINK_DISABLED,
};
static K_MUTEX_DEFINE(g_link_lock);

int hpi_npu_link_get(struct hpi_npu_link_info *out)
{
	if (out == NULL) {
		return -EINVAL;
	}
	k_mutex_lock(&g_link_lock, K_FOREVER);
	*out = g_link;
	k_mutex_unlock(&g_link_lock);

	return NPU_SPI_AVAILABLE ? 0 : -ENOTSUP;
}

#if NPU_SPI_AVAILABLE

/* Bumped by start() and stop(). A handshake that outlives its module drops its
 * result rather than overwrite the snapshot of the one that replaced it. */
static atomic_t npu_gen;
static atomic_val_t npu_run_gen;      /* the generation the handshake belongs to */
static int64_t npu_powered_ms;        /* uptime at start() */

static bool npu_stale(void)
{
	return atomic_get(&npu_gen) != npu_run_gen;
}

/* Publish a snapshot built on the stack. The lock is never held across a
 * transfer -- only across this copy. */
static void npu_link_publish(const struct hpi_npu_link_info *snap)
{
	k_mutex_lock(&g_link_lock, K_FOREVER);
	if (!npu_stale()) {
		g_link = *snap;
		g_link.checked_at_ms = k_uptime_get();
	}
	k_mutex_unlock(&g_link_lock);
}

static void npu_link_set_state(uint8_t state)
{
	k_mutex_lock(&g_link_lock, K_FOREVER);
	g_link.link_state = state;
	k_mutex_unlock(&g_link_lock);
}

static const struct device *const npu_spi = DEVICE_DT_GET(NPU_SPI_NODE);

/*
 * Chip-select follows the module, not the slot. Both SPI4 chip-selects reach
 * both slots (connector pin 9 CS_A = PE4, pin 10 CS_B = PE3), and a module
 * selects on the one its own board wires to NSS. The Compute module uses CS_A,
 * so PE4 is tried first in either slot and PE3 once as a fallback.
 *
 * CS is active-low, driven as a *plain GPIO* around each transfer. Do not route
 * it via spi_config.cs.gpio -- the soft-nss CS path faults.
 */
struct npu_cs_line {
	struct gpio_dt_spec cs;
	const char *name;
};

static const struct npu_cs_line npu_cs_lines[] = {
	{ .cs = { .port = DEVICE_DT_GET(DT_NODELABEL(gpioe)), .pin = 4,
		  .dt_flags = GPIO_ACTIVE_LOW },
	  .name = "PE4 (CS_A)" },
	{ .cs = { .port = DEVICE_DT_GET(DT_NODELABEL(gpioe)), .pin = 3,
		  .dt_flags = GPIO_ACTIVE_LOW },
	  .name = "PE3 (CS_B)" },
};

/* The CS line in use. Reset to CS_A by start(); moved by the fallback. */
static const struct npu_cs_line *npu_cur = &npu_cs_lines[0];

/*
 * Module IRQ to host: PI12 (aux GPIO 0), active-low. Like the CS lines, the aux
 * GPIOs reach both slots in parallel. In v2 the module asserts it when a REPLY
 * IS STAGED and releases it as that reply is clocked out.
 *
 * TREAT IT AS AN OPTIMISATION, NEVER A REQUIREMENT. Which pin this really is
 * has four answers that do not agree: this driver and the board overlay say
 * PI12 (with reset on PI2), the DF9 v3 connector table says host PH6 (reset
 * PI3), and the module's schematic-traced dtsi puts its own end on PB3 with
 * MOD_RESET_N at J5 pin 29 -- while that table's pin 19 is the module's USART1
 * RX. Until a human settles it against the schematic, a missing edge here must
 * cost latency and nothing else, so every wait falls through to a timeout and
 * the exchange proceeds regardless.
 */
static const struct gpio_dt_spec npu_irq = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpioi)),
	.pin = 12,
	.dt_flags = GPIO_ACTIVE_LOW | GPIO_PULL_UP,
};

static const struct spi_config npu_cfg = {
	/* Bring-up default 1 MHz (conservative -- rules out the >200 MHz-core
	 * CS/clock latch errata, zephyr#57219). Ratchet via Kconfig, not an
	 * edit here: 1 -> 8 -> 20 MHz as the link proves itself. */
	.frequency = CONFIG_HPI_NPU_SPI_FREQ_HZ,
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER,
	.slave = 0,
	/* no .cs -- npu_cur->cs toggled manually in npu_xfer_frame() */
};
static bool npu_cs_ready;
static bool npu_irq_ready;
/* Latched once the IRQ line is seen resting asserted, so the warning that says
 * so does not repeat on every command. */
static bool npu_irq_stuck;

/*
 * FIXED-LENGTH FRAMING (must match the module's CONFIG_HLC_SPI_FRAME_SIZE).
 *
 * The STM32 SPIv2 slave completes a transfer on EOT (the programmed TSIZE
 * count), NOT on NSS deassert, so the slave blocks forever unless every CS
 * transaction clocks exactly its armed buffer length. This is not a maximum --
 * it is the length. GET_INFO reports the module's own value so a mismatch is
 * diagnosable rather than a hang.
 */
#define NPU_SPI_FRAME_SIZE 256
static uint8_t npu_frame_tx[NPU_SPI_FRAME_SIZE];
static uint8_t npu_frame_rx[NPU_SPI_FRAME_SIZE];
static uint8_t npu_seq;

BUILD_ASSERT(NPU_SPI_FRAME_SIZE >= HLINK_OVERHEAD + HLINK_STATUS_LEN,
	     "the frame must hold the largest reply this host reads");

/* Clock one whole frame both ways. npu_frame_tx must already be built. */
static int npu_xfer_frame(void)
{
	if (npu_stale()) {
		return -ECANCELED;
	}

	if (!npu_cs_ready) {
		/* Drive BOTH CS lines inactive, not just ours: SPI4 is shared,
		 * and a floating CS could let another module answer over ours on
		 * MISO. */
		for (size_t i = 0; i < ARRAY_SIZE(npu_cs_lines); i++) {
			if (!device_is_ready(npu_cs_lines[i].cs.port)) {
				LOG_ERR("npu_xfer: CS port (gpioe) not ready");
				return -ENODEV;
			}
			if (gpio_pin_configure_dt(&npu_cs_lines[i].cs,
						  GPIO_OUTPUT_INACTIVE) != 0) {
				LOG_ERR("npu_xfer: CS %s configure failed",
					npu_cs_lines[i].name);
				return -EIO;
			}
		}
		npu_cs_ready = true;
	}

	memset(npu_frame_rx, 0, sizeof(npu_frame_rx));

	struct spi_buf txb = { .buf = npu_frame_tx, .len = NPU_SPI_FRAME_SIZE };
	struct spi_buf rxb = { .buf = npu_frame_rx, .len = NPU_SPI_FRAME_SIZE };
	struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
	struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

	gpio_pin_set_dt(&npu_cur->cs, 1);   /* assert (active-low: drives it low) */
	int rc = spi_transceive(npu_spi, &npu_cfg, &txs, &rxs);
	gpio_pin_set_dt(&npu_cur->cs, 0);   /* deassert */

	return rc;
}

/* Fill the outgoing frame with NOPs. 0x00 is both the correct pad and
 * HLINK_CMD_NOP; padding with 0xA5 would plant false SOFs in the module's scan
 * window. */
static void npu_frame_nop(void)
{
	memset(npu_frame_tx, HLINK_CMD_NOP, sizeof(npu_frame_tx));
}

/* Wait for the module to signal that a reply is staged. Returns 0 on assert,
 * -ETIMEDOUT otherwise -- and the caller proceeds either way (see npu_irq).
 * Without a usable IRQ the whole timeout is slept: that is the turnaround the
 * module needs, and reading sooner returns an all-zero frame.
 *
 * What is trusted is the ASSERT EDGE, never the level. The module releases PI12
 * whenever it has nothing staged, so a line already active on entry --
 * microseconds after the command frame finished clocking, before the module
 * could possibly have staged a reply to it -- is not this reply's assert. It is
 * a stale assert, or (far likelier, given the four-way pin disagreement above)
 * some other signal that simply rests low. Believing that level returns
 * immediately and collapses the turnaround to 0 ms, so all three read attempts
 * land ~10 ms after the command and every exchange reports NO_REPLY. An
 * already-active line therefore counts as no signal at all: sleep the full
 * timeout, exactly as when the pin cannot be configured. Costing latency and
 * nothing else is the whole contract of this line.
 */
static int npu_wait_reply_ready(int timeout_ms)
{
	if (npu_stale()) {
		return -ECANCELED;
	}

	if (!npu_irq_ready) {
		if (!device_is_ready(npu_irq.port) ||
		    gpio_pin_configure_dt(&npu_irq, GPIO_INPUT) != 0) {
			for (int waited = 0; waited < timeout_ms; waited += 2) {
				if (npu_stale()) {
					return -ECANCELED;
				}
				k_msleep(2);
			}
			return -ETIMEDOUT;
		}
		npu_irq_ready = true;
	}

	if (gpio_pin_get_dt(&npu_irq) == 1) {   /* already active: not an edge */
		if (!npu_irq_stuck) {
			npu_irq_stuck = true;
			LOG_WRN("NPU: IRQ asserted before a reply could be staged "
				"-- ignoring the line, waiting the full %d ms "
				"turnaround instead", timeout_ms);
		}
		for (int waited = 0; waited < timeout_ms; waited += 2) {
			if (npu_stale()) {
				return -ECANCELED;
			}
			k_msleep(2);
		}
		return -ETIMEDOUT;
	}
	npu_irq_stuck = false;

	for (int waited = 0; waited < timeout_ms; waited += 2) {
		if (npu_stale()) {
			return -ECANCELED;
		}
		k_msleep(2);
		if (gpio_pin_get_dt(&npu_irq) == 1) {   /* the assert edge */
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/*
 * One command, one reply.
 *
 * Replies are ONE TRANSACTION LATE: the slave only sees a command once the
 * frame carrying it has been fully clocked, by which point that transaction's
 * outgoing buffer has already gone. So this sends the command, waits for the
 * reply to be staged, then clocks a NOP frame to fetch it.
 *
 * The reply can start at ANY offset -- the N657 slave emits leading underrun
 * 0x00 bytes before a staged buffer -- so it is located by scanning for the SOF
 * and confirming the sequence number, never by assuming offset 0.
 *
 * On success `reply->payload` points into npu_frame_rx and is valid until the
 * next call.
 */
int npu_cmd(uint8_t cmd, const void *payload, uint16_t len,
	    struct hlink_frame *reply, enum npu_wait wait)
{
	uint8_t seq = ++npu_seq;

	if (seq == 0) {
		seq = ++npu_seq;   /* skip 0, so an all-zero window can't match */
	}

	memset(npu_frame_tx, HLINK_CMD_NOP, sizeof(npu_frame_tx));
	int n = hlink_encode(cmd, 0, seq, payload, len, npu_frame_tx,
			     sizeof(npu_frame_tx));
	if (n < 0) {
		return n;
	}

	int rc = npu_xfer_frame();   /* delivers the command; rx is stale, ignore */
	if (rc != 0) {
		if (rc != -ECANCELED) {
			LOG_ERR("NPU cmd 0x%02x: transceive failed (%d)", cmd, rc);
		}
		return rc;
	}

	int irq = -ETIMEDOUT;

	if (wait == NPU_WAIT_REPLY) {
		irq = npu_wait_reply_ready(CONFIG_HPI_NPU_IRQ_WAIT_MS);
		if (irq == -ECANCELED || npu_stale()) {
			return -ECANCELED;
		}
	} else {
		/* ACK: 2 ms default. Do not take the 50 ms IRQ path -- an
		 * already-active line there sleeps the full timeout. */
		if (npu_stale()) {
			return -ECANCELED;
		}
		if (CONFIG_HPI_NPU_ACK_WAIT_MS > 0) {
			k_msleep(CONFIG_HPI_NPU_ACK_WAIT_MS);
			if (npu_stale()) {
				return -ECANCELED;
			}
		}
	}

	/*
	 * Up to three read attempts, because "staged late" and "never staged"
	 * are indistinguishable from one look. NEVER resend the command -- the
	 * module would dispatch it twice.
	 */
	for (int attempt = 0; attempt < 3; attempt++) {
		if (npu_stale()) {
			return -ECANCELED;
		}
		npu_frame_nop();
		rc = npu_xfer_frame();
		if (rc != 0) {
			if (rc != -ECANCELED) {
				LOG_ERR("NPU cmd 0x%02x: reply transceive failed (%d)",
					cmd, rc);
			}
			return rc;
		}

		size_t scan = 0;

		while (scan + HLINK_HDR_LEN <= sizeof(npu_frame_rx)) {
			int rel = hlink_find_sof(npu_frame_rx + scan,
						 sizeof(npu_frame_rx) - scan);

			if (rel < 0) {
				break;
			}

			int off = (int)scan + rel;
			int d = hlink_decode(&npu_frame_rx[off],
					     sizeof(npu_frame_rx) - (size_t)off,
					     reply);

			if (d > 0 && reply->seq == seq &&
			    (reply->flags & HLINK_FLAG_REPLY) && reply->cmd == cmd) {
				LOG_DBG("NPU cmd 0x%02x: reply at +%d (%s, try %d)",
					cmd, off, irq == 0 ? "irq" : "timeout-fallback",
					attempt + 1);
				if (reply->flags & HLINK_FLAG_ERROR) {
					uint8_t st = reply->len ? reply->payload[0]
								: HLINK_ERR_INTERNAL;
					if (st == HLINK_ERR_PENDING) {
						return -EAGAIN;
					}
					/* Empty RESULT_POLL. The spec names this NO_RESULT;
					 * the 2.0.1 module maps the queue's
					 * -ENOENT to NO_MODEL. Neither is a
					 * fault worth a warning every poll. */
					if (st == HLINK_ERR_NO_RESULT ||
					    (cmd == HLINK_CMD_RESULT_POLL &&
					     st == HLINK_ERR_NO_MODEL)) {
						return -ENOENT;
					}
					LOG_WRN("NPU cmd 0x%02x: module says %s",
						cmd, hlink_status_str(st));
					return -EPROTO;
				}
				return 0;
			}
			LOG_DBG("NPU cmd 0x%02x: frame at +%d not ours (rc=%d)",
				cmd, off, d);
			scan = (size_t)off + 1;
		}
		if (npu_stale()) {
			return -ECANCELED;
		}
		k_msleep(5);
	}

	LOG_WRN("NPU cmd 0x%02x: no reply (seq %u)", cmd, seq);
	return -ETIMEDOUT;
}

/* ---- handshake ---- */

/*
 * Read the alive signature. The module pre-arms it at boot and re-arms it after
 * any command with no reply, so it is what an idle module clocks out.
 *
 * Version-tolerant on purpose: match the magic and the module id, then REPORT
 * what version it claims. The previous host compared all eight bytes including
 * the protocol major, so the v2 firmware's one changed byte read as a wiring
 * fault.
 */
static int npu_alive_probe(struct hpi_npu_link_info *snap)
{
	npu_frame_nop();

	int rc = npu_xfer_frame();

	if (rc != 0) {
		LOG_ERR("NPU: alive probe transceive failed (%d)", rc);
		return rc;
	}

	for (size_t i = 0; i + HLINK_ALIVE_LEN <= sizeof(npu_frame_rx); i++) {
		if (memcmp(&npu_frame_rx[i], hlink_alive_magic,
			   sizeof(hlink_alive_magic)) != 0) {
			continue;
		}
		const uint8_t *p = &npu_frame_rx[i];

		snap->proto_major = p[HLINK_ALIVE_OFF_MAJOR];
		snap->proto_minor = p[HLINK_ALIVE_OFF_MINOR];
		/* Big-endian here and nowhere else in this protocol. */
		snap->module_id = ((uint16_t)p[HLINK_ALIVE_OFF_ID_HI] << 8) |
				  p[HLINK_ALIVE_OFF_ID_LO];

		LOG_INF("NPU: alive signature at +%zu -- HLink %u.%u, module 0x%04x",
			i, snap->proto_major, snap->proto_minor, snap->module_id);

		if (snap->module_id != HEALTHYLINK_MODULE_ID_COMPUTE) {
			LOG_WRN("NPU: module id 0x%04x is not Compute (0x%04x)",
				snap->module_id, HEALTHYLINK_MODULE_ID_COMPUTE);
			return -ENODEV;
		}
		return 0;
	}

	/* A frame of one repeated byte is a MISO line nobody drives; anything
	 * else is a module answering out of phase or at the wrong rate. */
	size_t same = 1;

	while (same < sizeof(npu_frame_rx) && npu_frame_rx[same] == npu_frame_rx[0]) {
		same++;
	}
	if (same == sizeof(npu_frame_rx)) {
		LOG_WRN("NPU: no HLNK signature on CS %s; all %u bytes read 0x%02x "
			"-- MISO undriven", npu_cur->name,
			(unsigned int)sizeof(npu_frame_rx), npu_frame_rx[0]);
	} else {
		LOG_WRN("NPU: no HLNK signature, but MISO is toggling (CS %s) -- "
			"module out of phase, or the wrong clock rate?", npu_cur->name);
		LOG_HEXDUMP_WRN(npu_frame_rx, 32, "NPU rx (first 32 B)");
	}
	return -ENODEV;
}

/* Run once when CS_A drew nothing: a module wired to CS_B answers there. */
static int npu_alive_probe_other_cs(struct hpi_npu_link_info *snap)
{
	const struct npu_cs_line *first = npu_cur;

	for (size_t i = 0; i < ARRAY_SIZE(npu_cs_lines); i++) {
		if (&npu_cs_lines[i] == first || npu_stale()) {
			continue;
		}
		npu_cur = &npu_cs_lines[i];
		if (npu_alive_probe(snap) == 0) {
			LOG_WRN("NPU: the module answers on CS %s, not %s -- it is "
				"wired to the other chip-select pin. Using CS %s.",
				npu_cur->name, first->name, npu_cur->name);
			return 0;
		}
	}
	npu_cur = first;
	LOG_WRN("NPU: nothing answers on either CS line. Module firmware running? "
		"SPI4 SCK/MOSI/MISO reaching the slot?");
	return -ENODEV;
}

static void npu_decode_info(struct hpi_npu_link_info *snap, const uint8_t *p)
{
	snap->proto_major = p[HLINK_GI_OFF_PROTO_MAJOR];
	snap->proto_minor = p[HLINK_GI_OFF_PROTO_MINOR];
	snap->module_id = sys_get_le16(&p[HLINK_GI_OFF_MODULE_ID]);
	snap->fw_major = p[HLINK_GI_OFF_FW_MAJOR];
	snap->fw_minor = p[HLINK_GI_OFF_FW_MINOR];
	snap->fw_patch = p[HLINK_GI_OFF_FW_PATCH];
	snap->frame_size = sys_get_le16(&p[HLINK_GI_OFF_FRAME_SIZE]);
	snap->caps = sys_get_le32(&p[HLINK_GI_OFF_CAPS]);
	snap->info_valid = true;

	LOG_INF("NPU: HLink %u.%u, fw %u.%u.%u, frame %u B, caps 0x%08x",
		snap->proto_major, snap->proto_minor, snap->fw_major,
		snap->fw_minor, snap->fw_patch, snap->frame_size, snap->caps);

	if (snap->frame_size != 0 && snap->frame_size != NPU_SPI_FRAME_SIZE) {
		/* Not fatal here, but every subsequent transaction would hang
		 * the slave, so name it rather than let it present as dead HW. */
		LOG_ERR("NPU: module clocks %u B per CS, this host clocks %d. "
			"They must match exactly.",
			snap->frame_size, NPU_SPI_FRAME_SIZE);
	}
}

static void npu_decode_status(struct hpi_npu_link_info *snap, const uint8_t *p)
{
	snap->engine_state = p[HLINK_ST_OFF_ENGINE_STATE];
	snap->n_models = p[HLINK_ST_OFF_N_MODELS];
	memcpy(snap->active_name, &p[HLINK_ST_OFF_ACTIVE_NAME],
	       sizeof(snap->active_name) - 1);
	snap->active_name[sizeof(snap->active_name) - 1] = '\0';
	snap->runs_ok = sys_get_le16(&p[HLINK_ST_OFF_RUNS_OK]);
	snap->runs_failed = sys_get_le16(&p[HLINK_ST_OFF_RUNS_FAILED]);
	snap->err_crc = sys_get_le32(&p[HLINK_ST_OFF_ERR_CRC]);
	snap->err_dispatch = sys_get_le32(&p[HLINK_ST_OFF_ERR_DISPATCH]);
	snap->err_overrun = sys_get_le32(&p[HLINK_ST_OFF_ERR_OVERRUN]);
	snap->uptime_ms = sys_get_le32(&p[HLINK_ST_OFF_UPTIME_MS]);
	snap->status_valid = true;

	LOG_INF("NPU: engine=%u models=%u active='%s' runs=%u/%u "
		"err(crc=%u disp=%u ovr=%u) up=%u ms",
		snap->engine_state, snap->n_models, snap->active_name,
		snap->runs_ok, snap->runs_failed, snap->err_crc,
		snap->err_dispatch, snap->err_overrun, snap->uptime_ms);
}

/* The whole handshake. Runs on the npu_comms work queue, never on boot. */
static int npu_comms_check(void)
{
	struct hpi_npu_link_info snap = { 0 };
	struct hlink_frame reply;
	int rc;

	if (!device_is_ready(npu_spi)) {
		LOG_ERR("NPU comms: SPI4 controller not ready");
		snap.link_state = HPI_NPU_LINK_NO_SIGNATURE;
		snap.last_rc = -ENODEV;
		npu_link_publish(&snap);
		return -ENODEV;
	}

	/* Before the first transaction: prove the codec against its own
	 * reference vectors, so a CRC parameterisation mistake is one log line
	 * and not a bring-up week. */
	rc = hlink_selftest();
	if (rc != 0) {
		snap.link_state = HPI_NPU_LINK_NO_REPLY;
		snap.last_rc = rc;
		npu_link_publish(&snap);
		return rc;
	}

	LOG_INF("NPU comms: begin (SPI4 ready, freq=%u Hz, CS=%s, frame=%d B)",
		npu_cfg.frequency, npu_cur->name, NPU_SPI_FRAME_SIZE);

	/* A few looks, because a module that is still booting and one that is not
	 * there read the same from a single frame. */
	for (int attempt = 0; ; attempt++) {
		rc = npu_alive_probe(&snap);
		if (rc == 0 || attempt == 2 || npu_stale()) {
			break;
		}
		k_msleep(250);
	}
	if (rc != 0 && !npu_stale()) {
		rc = npu_alive_probe_other_cs(&snap);
	}
	if (rc != 0) {
		snap.link_state = HPI_NPU_LINK_NO_SIGNATURE;
		snap.last_rc = rc;
		npu_link_publish(&snap);
		return rc;
	}

	if (snap.proto_major < HLINK_PROTO_MAJOR) {
		/* A real module, running old firmware. Say exactly that; do not
		 * send it a command, because the numberings collide. */
		LOG_WRN("NPU comms: module speaks HLink v%u, this host speaks v%u. "
			"Reflash the module; no v1 commands will be sent.",
			snap.proto_major, HLINK_PROTO_MAJOR);
		snap.link_state = HPI_NPU_LINK_PROTO_MISMATCH;
		snap.last_rc = -EPROTONOSUPPORT;
		npu_link_publish(&snap);
		return -EPROTONOSUPPORT;
	}
	if (snap.proto_major > HLINK_PROTO_MAJOR) {
		/* Newer than us. The framing is stable across majors, so try --
		 * and record what it claims, so the log says why if it fails. */
		LOG_WRN("NPU comms: module speaks HLink v%u, newer than this host "
			"(v%u). Attempting the v%u handshake.",
			snap.proto_major, HLINK_PROTO_MAJOR, HLINK_PROTO_MAJOR);
	}

	rc = npu_cmd(HLINK_CMD_GET_INFO, NULL, 0, &reply, NPU_WAIT_REPLY);
	if (rc != 0 || reply.len < HLINK_GET_INFO_LEN) {
		if (rc != -ECANCELED && !npu_stale()) {
			LOG_WRN("NPU comms: GET_INFO failed (rc=%d, len=%u)", rc,
				rc == 0 ? reply.len : 0);
			snap.link_state = HPI_NPU_LINK_NO_REPLY;
			snap.last_rc = rc ? rc : -EBADMSG;
			npu_link_publish(&snap);
		}
		return rc ? rc : -EBADMSG;
	}
	npu_decode_info(&snap, reply.payload);

	rc = npu_cmd(HLINK_CMD_STATUS, NULL, 0, &reply, NPU_WAIT_REPLY);
	if (rc == 0 && reply.len >= HLINK_STATUS_LEN) {
		npu_decode_status(&snap, reply.payload);
	} else if (rc != -ECANCELED && !npu_stale()) {
		/* GET_INFO answered, so the link is up; STATUS is the richer
		 * reply and its absence is worth logging, not worth demoting
		 * the link for. status_valid stays false and every field it
		 * would have filled goes unshown. */
		LOG_WRN("NPU comms: STATUS failed (rc=%d) -- link is up, "
			"engine detail unavailable", rc);
	} else {
		return rc ? rc : -EBADMSG;
	}

	if (npu_stale()) {
		return -ECANCELED;
	}

	snap.link_state = HPI_NPU_LINK_UP;
	snap.last_rc = 0;
	npu_link_publish(&snap);
	LOG_INF("NPU comms: link UP");
	return 0;
}

/*
 * The handshake MUST NOT run on the boot/bring-up path: SPI4 transceive can
 * block indefinitely, and hl_framework_init() runs before main feeds the IWDG,
 * so a hang there is a boot loop. It runs once on a dedicated low-priority work
 * queue; selftest reports the cached result instead of re-running, so it cannot
 * wedge the MCUmgr thread either.
 */
K_THREAD_STACK_DEFINE(npu_wq_stack, 3072);   /* headroom: SPI driver + LOG args */
static struct k_work_q npu_wq;
static struct k_work npu_comms_work;
static bool npu_wq_started;

static void npu_comms_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	npu_run_gen = atomic_get(&npu_gen);

	/* The slot was powered only after identification, so the module is still
	 * booting. Wait in slices so a stop() ends the wait at once. */
	int64_t ready_at = npu_powered_ms + CONFIG_HPI_NPU_BOOT_WAIT_MS;

	while (k_uptime_get() < ready_at) {
		if (npu_stale()) {
			return;
		}
		k_msleep(50);
	}
	int rc = npu_comms_check();
#if IS_ENABLED(CONFIG_HPI_NPU_STREAM)
	if (rc == 0 && !npu_stale()) {
		npu_stream_on_link_up();
	}
#elif IS_ENABLED(CONFIG_HPI_NPU_INFER)
	if (rc == 0 && !npu_stale()) {
		npu_infer_on_link_up();
	}
#else
	ARG_UNUSED(rc);
#endif
}

static void npu_comms_kick(void)
{
	if (!npu_wq_started) {
		k_work_queue_init(&npu_wq);
		k_work_queue_start(&npu_wq, npu_wq_stack,
				   K_THREAD_STACK_SIZEOF(npu_wq_stack),
				   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
		k_thread_name_set(&npu_wq.thread, "npu_comms");
		k_work_init(&npu_comms_work, npu_comms_work_fn);
		npu_wq_started = true;
	}
	npu_link_set_state(HPI_NPU_LINK_IN_FLIGHT);
	k_work_submit_to_queue(&npu_wq, &npu_comms_work);
}

bool npu_link_stale(void)
{
	return npu_stale();
}

int npu_link_submit(struct k_work *work)
{
	if (!npu_wq_started) {
		return -ENODEV;
	}
	return k_work_submit_to_queue(&npu_wq, work);
}

void npu_link_cancel(struct k_work *work)
{
	(void)k_work_cancel(work);
}
#else
bool npu_link_stale(void)
{
	return true;
}

int npu_link_submit(struct k_work *work)
{
	ARG_UNUSED(work);
	return -ENODEV;
}

void npu_link_cancel(struct k_work *work)
{
	ARG_UNUSED(work);
}
#endif /* NPU_SPI_AVAILABLE */

static int npu_probe(struct hl_ctx *ctx)
{
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
	LOG_INF("NPU probe (slot %c): UART transport (USART2); reserving slot-A",
		'A' + ctx->slot);
#else
	LOG_INF("NPU probe (slot %c): claiming SPI4", 'A' + ctx->slot);
#endif
	return 0;
}

static int npu_start(struct hl_ctx *ctx)
{
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
	/* Parked transport, and it no longer reaches HLink on a v2 module: the
	 * module's UART link is bound to its USB CDC, and its USART1 carries
	 * MCUmgr. See include/healthylink/hl_npu_uart_proto.h. */
	LOG_INF("NPU start (slot %d): scheduling USART2 comms check (1 Mbaud)",
		ctx->slot);
	hpi_npu_uart_kick();
	return 0;
#elif NPU_SPI_AVAILABLE
	/* The same CS in either slot -- it follows the module (see npu_cs_lines). */
	npu_cur = &npu_cs_lines[0];
	npu_powered_ms = k_uptime_get();
	atomic_inc(&npu_gen);
	LOG_INF("NPU start (slot %c): scheduling the HLink v2 handshake off the "
		"boot path (SPI4, CS=%s, IRQ PI12, after %d ms boot)",
		'A' + ctx->slot, npu_cur->name, CONFIG_HPI_NPU_BOOT_WAIT_MS);
	npu_comms_kick();   /* runs on npu_comms wq; never blocks boot/watchdog */
	return 0;
#else
	ARG_UNUSED(ctx);
	LOG_INF("NPU start (slot %c): active; no comms transport enabled "
		"(set CONFIG_HPI_NPU_COMMS_CHECK)", 'A' + ctx->slot);
	return 0;
#endif
}

static int npu_stop(struct hl_ctx *ctx)
{
	LOG_INF("NPU stop (slot %c)", 'A' + ctx->slot);
#if NPU_SPI_AVAILABLE
	/* Orphan any running handshake and clear the snapshot. */
	atomic_inc(&npu_gen);
	if (npu_wq_started) {
		/* Do not wait: spi_transceive can hang, and this runs from the
		 * system workqueue (UI) and from MCUmgr. The handler polls
		 * npu_stale() before the next clock. */
#if IS_ENABLED(CONFIG_HPI_NPU_STREAM)
		npu_stream_cancel();
#endif
#if IS_ENABLED(CONFIG_HPI_NPU_INFER)
		npu_infer_cancel();
#endif
		(void)k_work_cancel(&npu_comms_work);
	}
	k_mutex_lock(&g_link_lock, K_FOREVER);
	g_link = (struct hpi_npu_link_info){ .link_state = HPI_NPU_LINK_NOT_RUN };
	k_mutex_unlock(&g_link_lock);
#endif
	return 0;
}

/*
 * Report the CACHED handshake -- never re-run it here. A hung SPI4 transfer
 * would wedge the MCUmgr handler thread, and this and the UI must agree, which
 * they can only do by reading the same snapshot.
 */
static int npu_selftest(struct hl_ctx *ctx, struct hl_test_result *out)
{
	ARG_UNUSED(ctx);
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
	switch (hpi_npu_uart_last_result()) {
	case 0:
		out->status = 0; /*PASS*/
		strncpy(out->detail, "NPU USART2 link OK", sizeof(out->detail) - 1);
		break;
	case -1:
		out->status = 2; /*SKIP*/
		strncpy(out->detail, "NPU comms check in flight", sizeof(out->detail) - 1);
		break;
	case -2:
		out->status = 2; /*SKIP*/
		strncpy(out->detail, "NPU comms not run", sizeof(out->detail) - 1);
		break;
	default:
		out->status = 1; /*FAIL*/
		strncpy(out->detail, "NPU USART2 no response", sizeof(out->detail) - 1);
		break;
	}
#else
	struct hpi_npu_link_info li;

	(void)hpi_npu_link_get(&li);
	switch (li.link_state) {
	case HPI_NPU_LINK_UP:
		out->status = 0; /*PASS*/
		snprintk(out->detail, sizeof(out->detail), "HLink %u.%u fw %u.%u.%u",
			 li.proto_major, li.proto_minor, li.fw_major, li.fw_minor,
			 li.fw_patch);
		break;
	case HPI_NPU_LINK_IN_FLIGHT:
		out->status = 2; /*SKIP*/
		strncpy(out->detail, "handshake in flight", sizeof(out->detail) - 1);
		break;
	case HPI_NPU_LINK_NOT_RUN:
		out->status = 2; /*SKIP*/
		strncpy(out->detail, "handshake not run", sizeof(out->detail) - 1);
		break;
	case HPI_NPU_LINK_DISABLED:
		out->status = 2; /*SKIP*/
		strncpy(out->detail, "SPI4 link not enabled", sizeof(out->detail) - 1);
		break;
	case HPI_NPU_LINK_PROTO_MISMATCH:
		/* A working link to firmware this host cannot drive. That is a
		 * real failure, and naming the version is the whole fix. */
		out->status = 1; /*FAIL*/
		snprintk(out->detail, sizeof(out->detail), "module speaks HLink v%u",
			 li.proto_major);
		break;
	case HPI_NPU_LINK_NO_SIGNATURE:
		out->status = 1; /*FAIL*/
		strncpy(out->detail, "no HLNK signature", sizeof(out->detail) - 1);
		break;
	default:
		out->status = 1; /*FAIL*/
		strncpy(out->detail, "no reply to GET_INFO", sizeof(out->detail) - 1);
		break;
	}
#endif
	out->detail[sizeof(out->detail) - 1] = '\0';
	return 0;
}

HL_MODULE_REGISTER(mod_npu) = {
	.module_id = HEALTHYLINK_MODULE_ID_COMPUTE,
	.name      = "HealthyLink Compute (STM32N657)",
	.caps      = HEALTHYLINK_CAP_REQUIRES_SPI4 | HEALTHYLINK_CAP_DMA_CAPABLE,
	.probe     = npu_probe,
	.start     = npu_start,
	.stop      = npu_stop,
	.selftest  = npu_selftest,
	.ctrl      = NULL,
};
