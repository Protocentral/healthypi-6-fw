/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * HealthyLink Compute (STM32N657) provider -- what the rest of the firmware may
 * ask about the module link.
 *
 * The provider itself is registered through hl_provider.h and needs no header;
 * this one exists for the ONE thing outside it that other code needs: a
 * snapshot of the HLink handshake. Both the group-64 self-test and the UI read
 * it, so neither can tell a different story than the other.
 *
 * The snapshot is written ONCE, off the boot path, when the module starts. It
 * is not polled and there is no rescan: a reader is looking at what the link
 * said at start-up, and anything that presents it must say so.
 */

#ifndef HPI_HEALTHYLINK_MOD_NPU_H
#define HPI_HEALTHYLINK_MOD_NPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How far the HLink handshake got. Every value is a distinct thing to say to a
 *  user; collapsing any two of them produces a misleading screen. */
enum hpi_npu_link_state {
	/** No handshake transport is compiled into this build. Not a failure. */
	HPI_NPU_LINK_DISABLED = 0,
	/** Never scheduled -- no module was detected in a slot. */
	HPI_NPU_LINK_NOT_RUN,
	/** Scheduled and running right now. */
	HPI_NPU_LINK_IN_FLIGHT,
	/** The bus worked; nothing that looks like a module answered on it. */
	HPI_NPU_LINK_NO_SIGNATURE,
	/** A module answered, speaking HLink v1. This host speaks v2 only. */
	HPI_NPU_LINK_PROTO_MISMATCH,
	/** Alive signature seen, but GET_INFO or STATUS did not come back. */
	HPI_NPU_LINK_NO_REPLY,
	/** Handshake complete. */
	HPI_NPU_LINK_UP,
};

/** The handshake snapshot. `info_valid` / `status_valid` gate the two halves --
 *  a field from a reply that never arrived is zero, and zero is a plausible
 *  reading for most of these. */
struct hpi_npu_link_info {
	uint8_t  link_state;      /**< enum hpi_npu_link_state */

	/* From the alive signature, then confirmed by GET_INFO. */
	uint8_t  proto_major;
	uint8_t  proto_minor;
	uint16_t module_id;

	/* GET_INFO (valid only when info_valid). */
	bool     info_valid;
	uint8_t  fw_major;
	uint8_t  fw_minor;
	uint8_t  fw_patch;
	uint16_t frame_size;      /**< bytes the module clocks per CS; 0 = non-SPI */
	uint32_t caps;            /**< HLINK_CAP_* -- the MODULE's claim, unedited */

	/* STATUS (valid only when status_valid). */
	bool     status_valid;
	uint8_t  engine_state;    /**< enum hlink_engine_state */
	uint8_t  n_models;
	char     active_name[32];
	uint16_t runs_ok;
	uint16_t runs_failed;
	uint32_t err_crc;
	uint32_t err_dispatch;
	uint32_t err_overrun;     /**< mixed units; "did it lose anything", not how much */
	uint32_t uptime_ms;

	int      last_rc;         /**< errno of the step that stopped the handshake */
	int64_t  checked_at_ms;   /**< k_uptime_get() when this was written */
};

/**
 * Copy the handshake snapshot.
 *
 * Mutex + struct copy, never held across a transfer -- the same contract as
 * hl_get_slot_status(), so it is safe to call from the LVGL thread at 2 Hz.
 *
 * @return 0, or -ENOTSUP when no handshake transport is compiled in (`out` is
 *         still filled, with link_state = HPI_NPU_LINK_DISABLED).
 */
int hpi_npu_link_get(struct hpi_npu_link_info *out);

#ifdef __cplusplus
}
#endif

#endif /* HPI_HEALTHYLINK_MOD_NPU_H */
