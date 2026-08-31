/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink Compute module provider (STM32N657 NPU, SPI slave).
 * Registers in the iterable provider section; the framework starts it when a
 * HealthyLink Compute module is detected.
 *
 * Bus: the NPU lives on **SPI4** (the HealthyLink slot-A high-speed bus), NOT
 * SPI6 -- touching SPI6 wedges the next SPI4 transceive. SPI4 is `st,soft-nss`
 * with NO cs-gpios, so the caller must supply the chip-select itself: slot A is
 * PE4 (CS1), slot B PE3 (CS2). spi_dt_spec is the wrong tool here (it would
 * carry no CS); this file drives the SPI4 controller directly.
 *
 * start() runs an SPI communications check with the NPU (STATUS + GET_INFO)
 * and reports the link state; the full inference round-trip (LOAD_INPUT ->
 * RUN_INFERENCE -> READ_OUTPUT, gated on the MOD_READY/IRQ line) builds on it.
 */

#include "hl_provider.h"
#include "npu_uart_host.h"             /* primary transport (USART2) */

#include <healthylink/healthylink.h>   /* module IDs + capability bits */
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(mod_npu, CONFIG_HPI_APP_LOG_LEVEL);

/* NPU SPI command set (mirrored by the N657 slave firmware). */
#define NPU_CMD_NOP       0x00   /* keep-alive; makes the slave re-arm HLNK sig */
#define NPU_CMD_STATUS    0x01   /* -> 1 status byte */
#define NPU_CMD_GET_INFO  0xF0   /* -> echo + INPUT_SIZE(2,BE) + OUTPUT_SIZE(1) + ver */
#define NPU_STATUS_BUSY   0x02   /* bit1: inference running */

/*
 * The N657 SPI slave is PIPELINED: it serves the reply to command N on
 * transaction N+1, and after any non-response command it re-arms tx_buffer with
 * an 8-byte "HLNK" alive signature. The module firmware is a SEPARATE
 * REPOSITORY: Protocentral/healthylink-compute-fw, app/src/spi_slave_handler.c,
 * with the contract written down in its docs/SPI_PROTOCOL.md. The constants
 * here are duplicated there by necessity -- changing one side alone does not
 * fail to build, it fails on the wire. So the host cannot read a reply in the
 * same transfer
 * that carries the command -- it must (1) send the command, (2) let the slave's
 * software loop process + re-post (~ms), (3) re-clock to read the reply. The
 * signature itself is the canonical proof-of-life. */
static const uint8_t npu_hlnk_sig[8] = { 'H', 'L', 'N', 'K', 0x01, 0x00, 0x00, 0x05 };

/*
 * Time for the slave's software loop to process a command and re-post its TX
 * buffer before we re-clock to read the reply. Reads earlier than this hit a
 * slave-side underrun (all-zero MISO); the MOD_READY/IRQ handshake is the
 * eventual replacement for this fixed delay.
 */
#define NPU_TURNAROUND_MS 50

#define NPU_SPI_NODE DT_NODELABEL(spi4)

/*
 * The SPI4 comms check is OFF by default (CONFIG_HPI_NPU_COMMS_CHECK): a
 * faulting transceive resets the MCU (boot loop). The NPU is still
 * detected/registered/active without it.
 */
#if DT_NODE_HAS_STATUS(NPU_SPI_NODE, okay) && IS_ENABLED(CONFIG_HPI_NPU_COMMS_CHECK)
#define NPU_SPI_AVAILABLE 1

static const struct device *const npu_spi = DEVICE_DT_GET(NPU_SPI_NODE);

/*
 * SPI4 is soft-NSS with no cs-gpios, so we own the chip-select: slot A = PE4
 * (CS1), active-low, driven as a *plain GPIO* around each transfer. Do not
 * route it via spi_config.cs.gpio -- the soft-nss CS path faults.
 */
static const struct gpio_dt_spec npu_cs = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpioe)),
    .pin = 4,
    .dt_flags = GPIO_ACTIVE_LOW,
};
static const struct spi_config npu_cfg = {
    /* Bring-up: 1 MHz (conservative -- rules out the >200 MHz-core CS/clock
     * latch errata, zephyr#57219). Bump back to 8 MHz once the link is proven. */
    .frequency = 1000000,
    .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER,
    .slave = 0,
    /* no .cs -- PE4 toggled manually in npu_xfer() */
};
static bool npu_cs_ready;

/* Module IRQ to host: slot A = PI12, active-low (overlay aux/irq-gpios). The
 * N657 asserts it LOW when inference output is ready, deasserts on READ_OUTPUT.
 * Driven directly as a plain input (the ai_accelerator DT node is deferred-init
 * and never claims the pin). */
static const struct gpio_dt_spec npu_irq = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpioi)),
    .pin = 12,
    .dt_flags = GPIO_ACTIVE_LOW | GPIO_PULL_UP,
};
static bool npu_irq_ready;

/* FIXED-LENGTH FRAMING (must match the N657 slave's NPU_SPI_FRAME_SIZE).
 *
 * The STM32 SPIv2 slave completes a transfer on EOT (TSIZE count), not on NSS
 * deassert, so the slave hangs unless every CS transaction clocks exactly its
 * armed buffer length. We therefore clock a fixed 256-byte frame each call: the
 * caller's payload (command or NOP read) goes at the front, the rest is padded
 * with NOP, and the slave's reply is read back from the front of the frame.
 * 256 B fits the largest command (LOAD_INPUT = 3 + 187). */
#define NPU_SPI_FRAME_SIZE 256
static uint8_t npu_frame_tx[NPU_SPI_FRAME_SIZE];
static uint8_t npu_frame_rx[NPU_SPI_FRAME_SIZE];

static int npu_xfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (len > NPU_SPI_FRAME_SIZE) {
        return -EINVAL;
    }
    if (!npu_cs_ready) {
        if (!device_is_ready(npu_cs.port)) {
            LOG_ERR("npu_xfer: CS port (gpioe) not ready");
            return -ENODEV;
        }
        if (gpio_pin_configure_dt(&npu_cs, GPIO_OUTPUT_INACTIVE) != 0) {
            LOG_ERR("npu_xfer: CS configure failed");
            return -EIO;
        }
        npu_cs_ready = true;
    }

    /* Build the fixed frame: payload at the front, NOP padding after. */
    memset(npu_frame_tx, NPU_CMD_NOP, NPU_SPI_FRAME_SIZE);
    if (tx != NULL && len > 0) {
        memcpy(npu_frame_tx, tx, len);
    }
    memset(npu_frame_rx, 0, NPU_SPI_FRAME_SIZE);

    struct spi_buf txb = { .buf = npu_frame_tx, .len = NPU_SPI_FRAME_SIZE };
    struct spi_buf rxb = { .buf = npu_frame_rx, .len = NPU_SPI_FRAME_SIZE };
    struct spi_buf_set txs = { .buffers = &txb, .count = 1 };
    struct spi_buf_set rxs = { .buffers = &rxb, .count = 1 };

    gpio_pin_set_dt(&npu_cs, 1);   /* assert (active-low: drives PE4 low) */
    int rc = spi_transceive(npu_spi, &npu_cfg, &txs, &rxs);
    gpio_pin_set_dt(&npu_cs, 0);   /* deassert */

    if (rc == 0 && rx != NULL && len > 0) {
        memcpy(rx, npu_frame_rx, len);   /* reply is at the front of the frame */
    }
    return rc;
}

/*
 * Issue one command, let the slave turn around, then re-clock to read its reply.
 * `cmd_tx`/`reply_rx` are independent buffers of `len` bytes; the second
 * (read-back) transaction sends NOPs so it does not trigger a new response.
 * Returns 0 only if both underlying transfers succeed.
 */
static int npu_cmd_reply(const uint8_t *cmd_tx, uint8_t *reply_rx, size_t len)
{
    uint8_t scratch[8] = { 0 };
    uint8_t nop[8];

    if (len > sizeof(nop)) {
        return -EINVAL;
    }
    memset(nop, NPU_CMD_NOP, sizeof(nop));

    int rc = npu_xfer(cmd_tx, scratch, len);   /* deliver command (ignore rx) */
    if (rc != 0) {
        return rc;
    }
    k_msleep(NPU_TURNAROUND_MS);               /* let the slave process + re-post */
    return npu_xfer(nop, reply_rx, len);       /* read the reply it staged */
}

#if IS_ENABLED(CONFIG_HPI_NPU_INFERENCE_TEST)
/* A classified beat is a signal like any other, so it leaves this file the same
 * way ECG does: published on the sample bus, where recording, the live stream
 * and the ESP32 link pick it up with no special-casing. healthylink/ is a
 * producer layer -- it may reach core/ and the HAL and nothing above. */
#include "core/sample_bus.h"
#include "core/channel_registry.h"
#include "core/sample_formats.h"

/* Beat-classifier model geometry (healthylink-compute-fw, app/src/npu_inference.h).
 * Kept in sync with the slave; GET_INFO reports the same at runtime. */
#define NPU_INPUT_SIZE  187   /* INT8 ECG samples @ 125 Hz */
#define NPU_OUTPUT_SIZE 5     /* AAMI class scores */

/*
 * Publish one classified beat.
 *
 * Every result this file emits carries HP6_INF_STUB, without exception. The
 * N657's RUN_INFERENCE is a stub that stages five zero bytes; nothing observable
 * from the host side -- not the IRQ, not the 0x30 marker, not a plausible score
 * vector -- distinguishes a real network run from that stub, so this code cannot
 * honestly claim one happened. The flag is what keeps a bring-up recording
 * separable from a clinical one months later, when the only evidence left is the
 * .HP6 file and nobody remembers which firmware the module was carrying. It is
 * cleared by the producer that can prove a network ran, and until such a producer
 * exists there is no correct path through here that leaves it clear.
 *
 * The input is a synthetic ramp besides, so the beat instant is meaningless as
 * physiology; it is recorded only to order this frame against the samples around
 * it.
 */
static void npu_publish_beat(const int8_t *scores)
{
    /* Argmax over the raw scores. Ties keep the first (lowest) class, which is
     * only reached when the network is genuinely undecided. */
    int best = 0;
    bool all_zero = (scores[0] == 0);

    for (int i = 1; i < NPU_OUTPUT_SIZE; i++) {
        if (scores[i] > scores[best]) {
            best = i;
        }
        if (scores[i] != 0) {
            all_zero = false;
        }
    }

    struct hp6_infer_sample s = {
        /* Uptime, matching every other producer's clock on this bus. */
        .ts_ms = (uint32_t)k_uptime_get(),
        /* GET_INFO reports input/output geometry and a protocol version, never a
         * model identity, so there is nothing truthful to put here yet. */
        .model_id = 0,
        .class_id = (uint8_t)best,
        /* int8 score -> the payload's 0..255 confidence: shift, do not scale, so
         * the byte stays reversible back to what the network returned. */
        .confidence = (uint8_t)((int)scores[best] + 128),
        .flags = HP6_INF_STUB,
    };
    memcpy(s.scores, scores, NPU_OUTPUT_SIZE);

    if (all_zero) {
        /* Five zeros IS the stub's signature. Argmax over them is a tie broken
         * by index alone, and index 0 is class N -- reporting "normal beat,
         * confidence 128" is the single most misleading thing this code could
         * emit, so say unclassifiable with no confidence instead. The raw scores
         * still travel in `scores[]`, so nothing is hidden from a consumer that
         * wants to see the zeros for itself. */
        s.class_id = (uint8_t)HP6_INF_CLASS_Q;
        s.confidence = 0;
        s.flags |= HP6_INF_LOW_CONF;
    }

    struct hpi_sample_frame f = {
        .channel = HPI_CH_INFER,
        /* Event-rate: one beat when a beat happens, not a sampled stream. */
        .sample_rate = 0,
        .sample_count = 1,
        .t_mono_us = (uint64_t)k_uptime_get() * 1000ULL,
        .len = sizeof(s),
        .flags = 0,
        .payload = &s,
    };
    (void)hpi_bus_publish(&f);

    LOG_INF("NPU infer: published class=%u conf=%u flags=0x%02x%s",
            s.class_id, s.confidence, s.flags,
            all_zero ? " (all-zero scores: module RUN_INFERENCE is a stub)" : "");
}

/* Wait for the module IRQ (PI12) to assert (inference done). Returns 0 on
 * assert, -ETIMEDOUT otherwise. Polled (no ISR) -- this runs on the npu_comms
 * work queue, off the boot/watchdog path. */
static int npu_wait_irq(int timeout_ms)
{
    if (!npu_irq_ready) {
        if (!device_is_ready(npu_irq.port) ||
            gpio_pin_configure_dt(&npu_irq, GPIO_INPUT) != 0) {
            LOG_ERR("NPU infer: IRQ (PI12) configure failed");
            return -EIO;
        }
        npu_irq_ready = true;
    }
    for (int waited = 0; waited <= timeout_ms; waited += 5) {
        if (gpio_pin_get_dt(&npu_irq) == 1) {   /* logical active = asserted */
            return 0;
        }
        k_msleep(5);
    }
    return -ETIMEDOUT;
}

/* one full inference round-trip with a synthetic input, to prove
 * LOAD_INPUT -> RUN_INFERENCE -> (IRQ) -> READ_OUTPUT end to end. */
static void npu_inference_test(void)
{
    /* LOAD_INPUT: CMD(1) + LEN(2,BE) + DATA(NPU_INPUT_SIZE) */
    uint8_t in[3 + NPU_INPUT_SIZE];
    uint8_t scratch[3 + NPU_INPUT_SIZE];
    in[0] = 0x10;                              /* AI_CMD_LOAD_INPUT */
    in[1] = (NPU_INPUT_SIZE >> 8) & 0xFF;
    in[2] = NPU_INPUT_SIZE & 0xFF;
    for (int i = 0; i < NPU_INPUT_SIZE; i++) { /* synthetic ramp test vector */
        in[3 + i] = (uint8_t)(i - 64);
    }
    LOG_INF("NPU infer: LOAD_INPUT %d B", NPU_INPUT_SIZE);
    if (npu_xfer(in, scratch, sizeof(in)) != 0) {
        LOG_WRN("NPU infer: LOAD_INPUT failed");
        return;
    }

    /* RUN_INFERENCE */
    uint8_t run[1] = { 0x20 };                 /* AI_CMD_RUN_INFERENCE */
    uint8_t r1[1];
    LOG_INF("NPU infer: RUN_INFERENCE");
    if (npu_xfer(run, r1, sizeof(run)) != 0) {
        LOG_WRN("NPU infer: RUN_INFERENCE failed");
        return;
    }

    /* Wait for the module to signal output ready (IRQ low). */
    int rc = npu_wait_irq(2000);
    if (rc != 0) {
        LOG_WRN("NPU infer: no IRQ within 2 s (rc=%d) -- inference/IRQ wiring?", rc);
        return;
    }
    LOG_INF("NPU infer: IRQ asserted (output ready)");

    /* READ_OUTPUT: deliver the command, then read the staged CMD(1)+LEN(2)+DATA
     * reply on the next transaction. The N657 SPI slave can emit one or more
     * leading underrun (0x00) bytes before its staged tx_buffer (same priming
     * race that mis-aligns GET_INFO), so read a window and SCAN for the 0x30
     * marker instead of assuming it lands at offset 0. */
    uint8_t ro_cmd[1] = { 0x30 };
    uint8_t scr1[1];
    if (npu_xfer(ro_cmd, scr1, sizeof(ro_cmd)) != 0) {
        LOG_WRN("NPU infer: READ_OUTPUT command failed");
        return;
    }
    k_msleep(NPU_TURNAROUND_MS);

    uint8_t win_tx[12];
    uint8_t win_rx[12] = { 0 };
    memset(win_tx, NPU_CMD_NOP, sizeof(win_tx));
    if (npu_xfer(win_tx, win_rx, sizeof(win_rx)) != 0) {
        LOG_WRN("NPU infer: READ_OUTPUT read failed");
        return;
    }
    LOG_INF("NPU infer: READ_OUTPUT raw = %02x %02x %02x %02x %02x %02x "
            "%02x %02x %02x %02x %02x %02x",
            win_rx[0], win_rx[1], win_rx[2], win_rx[3], win_rx[4], win_rx[5],
            win_rx[6], win_rx[7], win_rx[8], win_rx[9], win_rx[10], win_rx[11]);

    int idx = -1;
    for (int i = 0; i + 3 + NPU_OUTPUT_SIZE <= (int)sizeof(win_rx); i++) {
        if (win_rx[i] == 0x30) { idx = i; break; }
    }
    if (idx < 0) {
        LOG_WRN("NPU infer: READ_OUTPUT marker 0x30 not found "
                "(slave reply-priming -- N657 SPI TX needs pre-load)");
        return;
    }
    uint16_t out_len = ((uint16_t)win_rx[idx + 1] << 8) | win_rx[idx + 2];
    LOG_INF("NPU infer: OUTPUT %u B = %d %d %d %d %d (AAMI scores, marker@%d)",
            out_len, (int8_t)win_rx[idx + 3], (int8_t)win_rx[idx + 4],
            (int8_t)win_rx[idx + 5], (int8_t)win_rx[idx + 6],
            (int8_t)win_rx[idx + 7], idx);

    /* Only publish a beat we can actually parse. A length that is not the model's
     * output size means the reply we latched onto is not the one we think it is
     * (the same slave priming race that mis-aligns GET_INFO can put a stray 0x30
     * in the window), and a beat decoded from the wrong bytes is worse than no
     * beat -- it is indistinguishable from a real one downstream. */
    if (out_len != NPU_OUTPUT_SIZE) {
        LOG_WRN("NPU infer: OUTPUT len %u != %d -- reply misaligned, not published",
                out_len, NPU_OUTPUT_SIZE);
        return;
    }
    npu_publish_beat((const int8_t *)&win_rx[idx + 3]);

    LOG_INF("NPU infer: round-trip OK");
}
#endif /* CONFIG_HPI_NPU_INFERENCE_TEST */

/* Returns 0 if the NPU link is alive (HLNK signature echoes back). */
static int npu_comms_check(void)
{
    if (!device_is_ready(npu_spi)) {
        LOG_ERR("NPU comms: SPI4 controller not ready");
        return -ENODEV;
    }
    LOG_INF("NPU comms: begin (SPI4 ready, freq=%u Hz, CS=PE4)", npu_cfg.frequency);

    /*
     * Proof-of-life: the slave pre-arms its tx_buffer with the HLNK signature
     * at boot, so the FIRST transaction we clock returns it -- read it in a
     * single transfer (a second transfer risks an underrun of zeros before the
     * slave re-posts). A clean match proves the link end-to-end.
     */
    uint8_t nop[8];
    uint8_t sig_rx[8] = { 0 };
    memset(nop, NPU_CMD_NOP, sizeof(nop));
    int rc = npu_xfer(nop, sig_rx, sizeof(sig_rx));

    LOG_INF("NPU comms: alive-sig rx = %02x %02x %02x %02x %02x %02x %02x %02x",
            sig_rx[0], sig_rx[1], sig_rx[2], sig_rx[3],
            sig_rx[4], sig_rx[5], sig_rx[6], sig_rx[7]);

    if (rc != 0 || memcmp(sig_rx, npu_hlnk_sig, sizeof(npu_hlnk_sig)) != 0) {
        LOG_WRN("NPU comms FAIL -- no HLNK signature (rc=%d). Check N657 firmware "
                "running, SPI4 CS (PE4) and MISO wiring.", rc);
        return -EIO;
    }
    LOG_INF("NPU comms: link alive (HLNK signature OK)");

    /*
     * Bonus: GET_INFO reports the model geometry. Slave reply layout (one
     * transaction late): [0]=0xF0 echo, [1..2]=input size BE, [3]=output size,
     * [4]=protocol version.
     */
    uint8_t gi_tx[5] = { NPU_CMD_GET_INFO, 0, 0, 0, 0 };
    uint8_t gi_rx[5] = { 0 };
    if (npu_cmd_reply(gi_tx, gi_rx, sizeof(gi_rx)) == 0 && gi_rx[0] == NPU_CMD_GET_INFO) {
        uint16_t input_size = ((uint16_t)gi_rx[1] << 8) | gi_rx[2];
        LOG_INF("NPU comms OK -- GET_INFO input=%u output=%u proto_ver=%u",
                input_size, gi_rx[3], gi_rx[4]);
    } else {
        LOG_INF("NPU comms OK (alive) -- GET_INFO reply not aligned "
                "(rx0=0x%02x); link proven, model query needs slave timing tuning",
                gi_rx[0]);
    }

#if IS_ENABLED(CONFIG_HPI_NPU_INFERENCE_TEST)
    npu_inference_test();
#endif
    return 0;
}

/*
 * The comms check MUST NOT run on the boot/bring-up path: SPI4 transceive can
 * block indefinitely, and hl_framework_init() runs before main feeds the IWDG,
 * so a hang there is a boot loop. It runs once on a dedicated low-priority
 * work queue; selftest reports the cached result instead of re-running, so it
 * cannot wedge the MCUmgr thread either.
 */
static volatile int npu_last_comms = -2;   /* -2 = not run yet */

K_THREAD_STACK_DEFINE(npu_wq_stack, 3072);   /* headroom: SPI driver + LOG args */
static struct k_work_q npu_wq;
static struct k_work npu_comms_work;
static bool npu_wq_started;

static void npu_comms_work_fn(struct k_work *w)
{
    ARG_UNUSED(w);
    npu_last_comms = npu_comms_check();
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
    npu_last_comms = -1;   /* in flight */
    k_work_submit_to_queue(&npu_wq, &npu_comms_work);
}
#else
#define NPU_SPI_AVAILABLE 0
#endif /* NPU_SPI_NODE okay */

static int npu_probe(struct hl_ctx *ctx)
{
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
    LOG_INF("NPU probe (slot %d): UART transport (USART2); reserving slot-A",
            ctx->slot);
#else
    LOG_INF("NPU probe (slot %d): claiming SPI4", ctx->slot);
#endif
    return 0;
}

static int npu_start(struct hl_ctx *ctx)
{
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
    /* Primary transport: USART2 @ 1 Mbaud (SPI4 is parked). Comms check
     * runs off the boot path on its own work queue. */
    LOG_INF("NPU start (slot %d): scheduling USART2 comms check (1 Mbaud)",
            ctx->slot);
    hpi_npu_uart_kick();
    return 0;
#elif NPU_SPI_AVAILABLE
    LOG_INF("NPU start (slot %d): scheduling SPI4 comms check (CS=PE4) off the "
            "boot path", ctx->slot);
    npu_comms_kick();   /* runs on npu_comms wq; never blocks boot/watchdog */
    return 0;
#else
    ARG_UNUSED(ctx);
    LOG_INF("NPU start (slot %d): active; no comms transport enabled "
            "(set CONFIG_HPI_NPU_UART)", ctx->slot);
    return 0;
#endif
}

static int npu_stop(struct hl_ctx *ctx)
{
    LOG_INF("NPU stop (slot %d)", ctx->slot);
    return 0;
}

static int npu_selftest(struct hl_ctx *ctx, struct hl_test_result *out)
{
    ARG_UNUSED(ctx);
#if IS_ENABLED(CONFIG_HPI_NPU_UART)
    /* Report the cached UART comms-check result (set on the npu_uart wq). */
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
#elif NPU_SPI_AVAILABLE
    /* Report the cached async result; never re-run synchronously here (a hung
     * SPI4 transfer would wedge the MCUmgr handler thread). */
    switch (npu_last_comms) {
    case 0:
        out->status = 0; /*PASS*/
        strncpy(out->detail, "NPU SPI4 link OK", sizeof(out->detail) - 1);
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
        strncpy(out->detail, "NPU SPI4 no/stuck response", sizeof(out->detail) - 1);
        break;
    }
#else
    out->status = 2;   /* SKIP */
    strncpy(out->detail, "SPI4 not enabled", sizeof(out->detail) - 1);
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
