/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HLink v2 -- host half of the HealthyPi 6 <-> HealthyLink Compute wire
 * protocol. Framing, CRC and the handshake command subset only.
 *
 * THE OTHER HALF IS IN A DIFFERENT REPOSITORY.
 *   Protocentral/healthylink-compute-fw
 *     docs/HLINK_PROTOCOL.md        -- the specification (sections cited below)
 *     app/src/link/hlink_proto.{c,h}-- the module's codec; this file mirrors it
 *     tools/hlc/hlc/hlink_host.py   -- a third implementation, the PC harness
 *     tests/src/test_hlink_dispatch.c -- 39 cases asserting these byte offsets
 *
 * Every constant here is duplicated there by necessity. A change on one side
 * alone does not fail to build -- it fails on the wire, at a customer, months
 * later. Change both together, and keep the function names identical so the
 * two files can be diffed.
 *
 * Data-plane payload layouts (MODEL, TENSOR, STREAM) are mirrored here so
 * npu_infer.c / npu_stream.c can encode without inventing offsets. There is no
 * FILE_* (0x40) block: the module kept SMP for management.
 *
 * v1 NOTE. The host spoke a different protocol until this file existed: a bare
 * command byte at offset 0, no SOF, no CRC, no sequence number, and a numbering
 * that COLLIDES with this one (v1 STATUS 0x01 is v2 PING; v1 LOAD_INPUT 0x10 is
 * MODEL_LIST; v1 RUN_INFERENCE 0x20 is TENSOR_LOAD; v1 READ_OUTPUT 0x30 is
 * STREAM_PUSH). There is no fallback path and there must never be one -- two
 * live numberings in one file is how a plausible wrong answer gets produced.
 * A v1 module is detected by its alive signature and reported, never driven.
 */

#ifndef HPI_HEALTHYLINK_HLINK_PROTO_H
#define HPI_HEALTHYLINK_HLINK_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Framing (HLINK_PROTOCOL.md §3)
 *
 *   0 sof(0xA5) | 1 cmd | 2 flags | 3 seq | 4-5 len u16 LE | 6-7 rsvd u16 LE
 *   | 8.. payload | crc16 u16 LE
 *
 * The CRC covers bytes [1, 8+len) -- the header as well as the payload, so a
 * corrupted `seq` or `cmd` is caught rather than acted on.
 * ------------------------------------------------------------------------- */

#define HLINK_SOF          0xA5u
#define HLINK_UART_SOF2    0xC3u   /**< UART preamble byte 2; unused over SPI */
#define HLINK_HDR_LEN      8u
#define HLINK_CRC_LEN      2u
#define HLINK_OVERHEAD     (HLINK_HDR_LEN + HLINK_CRC_LEN)

/* Header field offsets, named because the same numbers appear in the module's
 * link_uart.c parser and in tools/hlc. */
#define HLINK_OFF_SOF      0u
#define HLINK_OFF_CMD      1u
#define HLINK_OFF_FLAGS    2u
#define HLINK_OFF_SEQ      3u
#define HLINK_OFF_LEN      4u
#define HLINK_OFF_RSVD     6u
#define HLINK_OFF_PAYLOAD  8u

/** CRC16-CCITT / IBM-3740 seed. See hlink_crc16(). */
#define HLINK_CRC16_INIT   0xFFFFu

/** Header `flags` bits. */
enum hlink_flag {
	HLINK_FLAG_REPLY = 0x01u, /**< reply to the request with the same seq */
	HLINK_FLAG_ERROR = 0x02u, /**< payload is one status byte, nothing else */
	HLINK_FLAG_MORE  = 0x04u, /**< more frames follow (chunked transfers) */
};

/** Command codes. Values are fixed; new ones may be added, never renumbered. */
enum hlink_cmd {
	HLINK_CMD_NOP      = 0x00, /**< no operation; clocks out a staged reply */
	HLINK_CMD_PING     = 0x01, /**< liveness; echoes its payload */
	HLINK_CMD_GET_INFO = 0x02, /**< proto ver, module id, fw ver, frame, caps */
	HLINK_CMD_STATUS   = 0x03, /**< engine state + error counters */
	HLINK_CMD_RESET    = 0x04, /**< reset the link state machine */

	/* Data plane. Layouts below; this host does not send them yet. */
	HLINK_CMD_MODEL_LIST       = 0x10,
	HLINK_CMD_MODEL_INFO       = 0x11,
	HLINK_CMD_MODEL_ACTIVATE   = 0x12,
	HLINK_CMD_MODEL_DEACTIVATE = 0x13,
	HLINK_CMD_TENSOR_LOAD      = 0x20,
	HLINK_CMD_RUN              = 0x21,
	HLINK_CMD_READ_RESULT      = 0x22,
	HLINK_CMD_STREAM_PUSH      = 0x30,
	HLINK_CMD_STREAM_EVENT     = 0x31,
	HLINK_CMD_RESULT_POLL      = 0x32,
};

/** Status codes carried in payload[0] of an HLINK_FLAG_ERROR reply (§5). */
enum hlink_status {
	HLINK_OK                = 0,
	HLINK_ERR_BAD_CMD       = 1,
	HLINK_ERR_BAD_LEN       = 2,
	HLINK_ERR_BAD_CRC       = 3,
	HLINK_ERR_NOT_READY     = 4,
	HLINK_ERR_NO_MODEL      = 5,
	HLINK_ERR_BUSY          = 6,
	HLINK_ERR_UNSUPPORTED   = 7,
	HLINK_ERR_INTERNAL      = 8,
	HLINK_ERR_PENDING       = 9,
	HLINK_ERR_NO_RESULT     = 10,
	HLINK_ERR_RANGE         = 11,
};

/** A decoded frame. `payload` points into the caller's buffer; nothing copies. */
struct hlink_frame {
	uint8_t        cmd;
	uint8_t        flags;
	uint8_t        seq;
	uint16_t       len;
	uint16_t       rsvd;
	const uint8_t *payload;
};

/* ---------------------------------------------------------------------------
 * Alive signature (§7)
 *
 * Pre-armed in the module's TX buffer at start-up and re-armed after any
 * command with no reply, so it is what a host clocks out of an idle module.
 *
 *   'H' 'L' 'N' 'K'  proto_major  proto_minor  module_id_hi  module_id_lo
 *
 * The module id is BIG-endian here and little-endian everywhere else in this
 * protocol. That is inherited from v1 and deliberate: it is what the slot scan
 * matched on, so detection had to keep working across the rewrite.
 * ------------------------------------------------------------------------- */
#define HLINK_ALIVE_LEN          8u
#define HLINK_ALIVE_OFF_MAJOR    4u
#define HLINK_ALIVE_OFF_MINOR    5u
#define HLINK_ALIVE_OFF_ID_HI    6u
#define HLINK_ALIVE_OFF_ID_LO    7u
extern const uint8_t hlink_alive_magic[4];   /**< "HLNK" */

/** The protocol major this host speaks. A module reporting 1 is not driven. */
#define HLINK_PROTO_MAJOR        2u

/* ---------------------------------------------------------------------------
 * GET_INFO reply -- 14 bytes, fixed (§4.1)
 * ------------------------------------------------------------------------- */
#define HLINK_GET_INFO_LEN        14u
#define HLINK_GI_OFF_PROTO_MAJOR  0u
#define HLINK_GI_OFF_PROTO_MINOR  1u
#define HLINK_GI_OFF_MODULE_ID    2u   /**< u16 LE */
#define HLINK_GI_OFF_FW_MAJOR     4u
#define HLINK_GI_OFF_FW_MINOR     5u
#define HLINK_GI_OFF_FW_PATCH     6u
#define HLINK_GI_OFF_FRAME_SIZE   8u   /**< u16 LE; 0 on non-SPI transports */
#define HLINK_GI_OFF_CAPS         10u  /**< u32 LE */

/** GET_INFO `caps` bits. Each is computed from the subsystem's own state at
 *  reply time, so it means "this is wired", not "this was compiled in". */
enum hlink_caps {
	HLINK_CAP_ENGINE   = 0x01u,
	HLINK_CAP_STREAM   = 0x02u,
	HLINK_CAP_REGISTRY = 0x04u,
	HLINK_CAP_RESULTS  = 0x08u,
};

/* ---------------------------------------------------------------------------
 * STATUS reply -- 66 bytes (§4.2)
 * ------------------------------------------------------------------------- */
#define HLINK_STATUS_LEN            66u
#define HLINK_NAME_LEN              32u
#define HLINK_VER_LEN               16u
#define HLINK_LABEL_LEN             8u
#define HLINK_MAX_DIMS              6u
#define HLINK_MAX_LABELS            16u
#define HLINK_CHANNEL_LEN           16u
#define HLINK_ST_OFF_ENGINE_STATE   0u
#define HLINK_ST_OFF_N_MODELS       1u
#define HLINK_ST_OFF_ACTIVE_IDX     2u   /**< 0xFF when none */
#define HLINK_ST_OFF_FLAGS          3u
#define HLINK_ST_OFF_ACTIVE_NAME    4u   /**< char[32], NUL-padded */
#define HLINK_ST_OFF_PENDING_RES    36u
#define HLINK_ST_OFF_RESULT_Q_DEPTH 37u
#define HLINK_ST_OFF_RUNS_OK        38u  /**< u16 LE */
#define HLINK_ST_OFF_RUNS_FAILED    40u  /**< u16 LE */
#define HLINK_ST_OFF_ERR_CRC        42u  /**< u32 LE */
#define HLINK_ST_OFF_ERR_DISPATCH   46u  /**< u32 LE */
#define HLINK_ST_OFF_ERR_OVERRUN    50u  /**< u32 LE -- see the note below */
#define HLINK_ST_OFF_MODEL_RAM_USED 54u  /**< u32 LE */
#define HLINK_ST_OFF_MODEL_RAM_TOT  58u  /**< u32 LE */
#define HLINK_ST_OFF_UPTIME_MS      62u  /**< u32 LE, wraps at ~49.7 days */

#define HLINK_ACTIVE_IDX_NONE       0xFFu

/*
 * err_overrun MIXES UNITS ON PURPOSE and must never be shown as a quantity.
 * It sums samples a channel writer discarded, samples in pushes refused
 * outright, stream events lost to a full ring, results overwritten before the
 * host read them, and loss events on the window path -- where a sliding-window
 * lap counts 1 no matter how many windows it stepped over. It answers "did this
 * module lose anything?", not "how much". (Widened 2026-09-09 to include the
 * window term; same offset, same width, so an older host still decodes it.)
 */

/** STATUS engine_state values. */
enum hlink_engine_state {
	HLINK_ENGINE_DOWN = 0,
	HLINK_ENGINE_IDLE = 1,
	HLINK_ENGINE_BUSY = 2,
};

/** STATUS flags bits. */
enum hlink_status_flag {
	HLINK_ST_STREAM_READY = 0x01u,
	HLINK_ST_SCHED_READY  = 0x02u,
	HLINK_ST_TENSOR_PART  = 0x04u,
	HLINK_ST_MODEL_ACTIVE = 0x08u,
};

#define HLINK_MODEL_IDX_ACTIVE  0xFFu  /**< TENSOR_LOAD / RUN: the active model */
#define HLINK_RUN_ID_LATEST     0xFFFFu

/* ---------------------------------------------------------------------------
 * MODEL_LIST -- paged (HLINK_PROTOCOL.md §4.3)
 *
 * Request: start index, 1 byte. Reply header 4 B + count * 56 B entries.
 * MORE when start + count < total.
 * ------------------------------------------------------------------------- */
#define HLINK_MODEL_LIST_HDR_LEN    4u
#define HLINK_MODEL_LIST_ENTRY_LEN  56u

struct hlink_model_list_hdr {
	uint8_t total;
	uint8_t start;
	uint8_t count;
	uint8_t rsvd;
} __attribute__((packed));

struct hlink_model_list_entry {
	char     name[HLINK_NAME_LEN];
	char     version[HLINK_VER_LEN];
	uint32_t size;
	uint8_t  ok;     /**< 0 = installable; else hlm_validate errno magnitude */
	uint8_t  active;
	uint8_t  rsvd[2];
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * MODEL_INFO -- two parts (HLINK_PROTOCOL.md §4.4)
 *
 * Request: name[32] + part (0 descriptor, 1 labels).
 * ------------------------------------------------------------------------- */
#define HLINK_MODEL_INFO_REQ_LEN     33u
#define HLINK_TENSOR_DESC_LEN        36u
#define HLINK_MODEL_INFO_PART0_LEN   161u

struct hlink_model_info_req {
	char    name[HLINK_NAME_LEN];
	uint8_t part;
} __attribute__((packed));

struct hlink_tensor_desc {
	uint8_t  dtype;      /**< 1 s8, 2 u8, 3 s16, 4 f32 */
	uint8_t  n_dims;
	uint8_t  rsvd[2];
	uint32_t dims[HLINK_MAX_DIMS];
	float    scale;
	int32_t  zero_point;
} __attribute__((packed));

struct hlink_model_info_part0 {
	char     name[HLINK_NAME_LEN];
	char     version[HLINK_VER_LEN];
	uint8_t  active;
	uint8_t  n_inputs;
	uint8_t  n_outputs;
	uint8_t  post_kind;  /**< 0 none, 1 argmax, 2 softmax */
	struct hlink_tensor_desc in;
	struct hlink_tensor_desc out;
	uint32_t rate_hz;
	uint32_t window;
	uint32_t hop;
	uint8_t  window_mode;  /**< 0 sliding, 1 event-triggered */
	uint8_t  trigger_kind; /**< 1 = beat / R-peak */
	uint16_t pre_trigger;
	char     channel[HLINK_CHANNEL_LEN];
	float    threshold;
	uint8_t  n_labels;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * TENSOR_LOAD -- chunked (HLINK_PROTOCOL.md §4.6)
 *
 * Request: 4-byte header then the chunk. Reply 8 B.
 * ------------------------------------------------------------------------- */
#define HLINK_TENSOR_LOAD_REQ_HDR_LEN 4u
#define HLINK_TENSOR_LOAD_RSP_LEN     8u

struct hlink_tensor_load_req {
	uint8_t  model_idx;
	uint8_t  input_idx;
	uint16_t offset;
	/* followed by chunk bytes */
} __attribute__((packed));

struct hlink_tensor_load_rsp {
	uint8_t  status;
	uint8_t  model_idx;
	uint16_t offset;
	uint16_t written;
	uint16_t total;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * RUN (HLINK_PROTOCOL.md §4.7) -- reply means queued, not finished
 * ------------------------------------------------------------------------- */
#define HLINK_RUN_REQ_LEN 1u
#define HLINK_RUN_RSP_LEN 4u

struct hlink_run_req {
	uint8_t model_idx;
} __attribute__((packed));

struct hlink_run_rsp {
	uint8_t  status;
	uint8_t  model_idx;
	uint16_t run_id;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * READ_RESULT / RESULT_POLL (HLINK_PROTOCOL.md §4.8)
 *
 * One layout for both. PENDING vs NO_RESULT are different ERROR codes.
 * ------------------------------------------------------------------------- */
#define HLINK_READ_RESULT_REQ_LEN 4u
#define HLINK_RESULT_HDR_LEN      36u

struct hlink_read_result_req {
	uint16_t run_id;     /**< 0xFFFF = most recent */
	uint16_t out_offset;
} __attribute__((packed));

struct hlink_result_poll_req {
	uint16_t out_offset;
} __attribute__((packed));

enum hlink_result_flag {
	HLINK_RES_LOW_CONF = 0x01u,
	HLINK_RES_STREAM   = 0x02u,
};

struct hlink_result_hdr {
	uint16_t run_id;
	uint8_t  model_idx;
	uint8_t  run_status;  /**< 0 ran; else errno magnitude */
	uint8_t  argmax;
	uint8_t  confidence;
	uint8_t  flags;       /**< HLINK_RES_* */
	uint8_t  n_out;
	char     label[HLINK_LABEL_LEN];
	uint32_t cycles;
	uint32_t us;
	uint64_t t_ms;
	uint16_t out_total;
	uint16_t out_offset;
	/* followed by n_out raw output bytes */
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * STREAM_PUSH (HLINK_PROTOCOL.md §4.9)
 *
 * channel is the module's number, not an HP6 bus id:
 *   0 resp, 1 lead I, 2 lead II, 3 V1, 4 PPG red, 5 PPG IR, 8-15 EEG.
 * A manifest "channel: ecg" is lead II (2), never respiration (0).
 *
 * Payload after the 12-byte header is n samples of that channel only --
 * int32 µV LE at 500 Hz for ECG -- NOT the 20-byte hp6_ecg_sample struct.
 * ------------------------------------------------------------------------- */
#define HLINK_STREAM_PUSH_REQ_HDR_LEN 12u
#define HLINK_STREAM_PUSH_RSP_LEN     8u

enum hlink_stream_ch {
	HLINK_STREAM_CH_ECG_RESP = 0,
	HLINK_STREAM_CH_ECG_I    = 1,
	HLINK_STREAM_CH_ECG_II   = 2, /**< beat classifier input */
	HLINK_STREAM_CH_ECG_V1   = 3,
	HLINK_STREAM_CH_PPG_RED  = 4,
	HLINK_STREAM_CH_PPG_IR   = 5,
};

enum hlink_stream_fmt {
	HLINK_STREAM_FMT_I32 = 0, /**< int32 LE, ECG µV */
	HLINK_STREAM_FMT_I16 = 1,
	HLINK_STREAM_FMT_I8  = 2,
};

struct hlink_stream_push_req {
	uint8_t  channel;
	uint8_t  format;
	uint16_t n;
	uint64_t t_ms; /**< timestamp of the FIRST sample */
	/* followed by n samples */
} __attribute__((packed));

struct hlink_stream_push_rsp {
	uint8_t  status;
	uint8_t  channel;
	uint16_t accepted;
	uint32_t dropped;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * STREAM_EVENT (HLINK_PROTOCOL.md §4.10)
 * ------------------------------------------------------------------------- */
#define HLINK_STREAM_EVENT_REQ_LEN 12u
#define HLINK_STREAM_EVENT_RSP_LEN 4u
#define HLINK_STREAM_EVENT_BEAT    1u

struct hlink_stream_event_req {
	uint8_t  kind;
	uint8_t  rsvd0;
	uint16_t rsvd1;
	uint64_t t_ms;
} __attribute__((packed));

struct hlink_stream_event_rsp {
	uint8_t  status;
	uint8_t  kind;
	uint16_t windows_queued;
} __attribute__((packed));

/* ---------------------------------------------------------------------------
 * Codec
 * ------------------------------------------------------------------------- */

/**
 * CRC16-CCITT / IBM-3740: poly 0x1021, init 0xFFFF, NO reflection of input or
 * output, no final xor. Check value 0x29B1 over the ASCII "123456789".
 *
 * Pass the running value back in as `crc` to span non-contiguous blocks.
 *
 * READ THIS BEFORE CHANGING IT: Zephyr's crc16_ccitt() is NOT this function --
 * it is the REFLECTED 0x1021 variant (CRC-16/KERMIT at seed 0, CRC-16/X-25 at
 * seed 0xFFFF). The non-reflected one is crc16_itu_t(). Getting this wrong
 * rejects every frame with a symptom indistinguishable from bad wiring, which
 * is why hlink_selftest() asserts the check value on the way up.
 */
uint16_t hlink_crc16(uint16_t crc, const uint8_t *data, size_t len);

/**
 * Encode one frame into `out`.
 *
 * @return the whole frame length (HLINK_OVERHEAD + len), or
 *         -EINVAL (NULL out, or payload NULL with len > 0),
 *         -ENOSPC (out_cap too small).
 */
int hlink_encode(uint8_t cmd, uint8_t flags, uint8_t seq,
		 const void *payload, uint16_t len, uint8_t *out, size_t out_cap);

/**
 * Decode the frame starting at buf[0].
 *
 * @return the whole frame length, or
 *         -EINVAL (NULL argument), -EBADMSG (no SOF at buf[0]),
 *         -EAGAIN (buffer shorter than the frame it declares),
 *         -EILSEQ (CRC mismatch).
 */
int hlink_decode(const uint8_t *buf, size_t len, struct hlink_frame *out);

/**
 * Find the offset of a plausible frame: an SOF byte whose declared length still
 * fits the window. Does NOT check the CRC -- hlink_decode() does that.
 *
 * This is what tolerates the module's leading underrun bytes: the STM32N6 SPI
 * slave clocks some number of 0x00s before a staged buffer, so a reply can
 * start at any offset and a host must never assume 0.
 *
 * @return the offset, or -ENOENT.
 */
int hlink_find_sof(const uint8_t *buf, size_t len);

/** Name a status code for a log line ("BAD_CRC"). Never NULL. */
const char *hlink_status_str(uint8_t status);

/**
 * Check the codec against its own reference vectors: the 0x29B1 CRC check value
 * and one encode -> decode round-trip. Run once before the first transaction.
 *
 * @return 0, or -EILSEQ if the CRC parameterisation is wrong.
 */
int hlink_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_HLINK_PROTO_H */
