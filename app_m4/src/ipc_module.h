/*
 * Copyright (c) 2024 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef IPC_MODULE_H
#define IPC_MODULE_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/logging/log.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IPC Message Types */
enum hpi_ipc_msg_type {
	HPI_IPC_MSG_TYPE_TEST = 0x01,
	HPI_IPC_MSG_TYPE_DATA = 0x02,      // Generic data (deprecated for new designs)
	HPI_IPC_MSG_TYPE_CMD  = 0x03,
	HPI_IPC_MSG_TYPE_STATUS = 0x04,
	HPI_IPC_MSG_TYPE_VERSION = 0x05,   // M4→M7: firmware version (sent at bind)
	
	/* PPG/SpO2 Algorithm Message Types */
	HPI_IPC_MSG_TYPE_PPG_RAW = 0x10,    // M7→M4: Raw PPG samples (Red+IR)
	HPI_IPC_MSG_TYPE_PPG_VITALS = 0x11, // M4→M7: Calculated SpO2+HR
	HPI_IPC_MSG_TYPE_PPG_CONFIG = 0x12, // M7→M4: Algorithm configuration
	
	/* ECG/QRS Algorithm Message Types */
	HPI_IPC_MSG_TYPE_ECG_RAW = 0x20,    // M7→M4: Raw ECG samples (3-lead)
	HPI_IPC_MSG_TYPE_ECG_VITALS = 0x21, // M4→M7: Calculated HR+HRV+QRS
	HPI_IPC_MSG_TYPE_ECG_CONFIG = 0x22, // M7→M4: Algorithm configuration
	HPI_IPC_MSG_TYPE_BEAT_NOTIFY = 0x23, // M4→M7: Beat detected (sample index only; unused)

	/* EEG/Mental State Algorithm Message Types */
	HPI_IPC_MSG_TYPE_EEG_RAW = 0x30,    // M7→M4: Raw EEG samples (ADC channels)
	HPI_IPC_MSG_TYPE_EEG_VITALS = 0x31, // M4→M7: Band powers+mental state
	HPI_IPC_MSG_TYPE_EEG_CONFIG = 0x32, // M7→M4: Algorithm configuration

	/* Arrhythmia/TFLite Classification Message Types */
	HPI_IPC_MSG_TYPE_ARRHYTHMIA_RESULT = 0x40,  // M4→M7: Per-beat classification
	HPI_IPC_MSG_TYPE_ARRHYTHMIA_STATS = 0x41,   // M4→M7: Periodic statistics
	HPI_IPC_MSG_TYPE_ARRHYTHMIA_ALERT = 0x42,   // M4→M7: Threshold alerts
	HPI_IPC_MSG_TYPE_ARRHYTHMIA_CONFIG = 0x43,  // M7→M4: Classification config
	HPI_IPC_MSG_TYPE_BEAT_WAVEFORM = 0x44,      // M4→M7: Beat waveform + classification
	HPI_IPC_MSG_TYPE_TFLITE_DEBUG = 0x45,       // M4→M7: TFLite debug info for diagnostics

	HPI_IPC_MSG_TYPE_MAX
};

/* IPC Message Structure */
struct hpi_ipc_msg {
	uint8_t type;
	uint8_t reserved;
	uint16_t length;
	uint8_t data[];
} __packed;

/* Maximum message size */
#define HPI_IPC_MAX_MSG_SIZE 512
#define HPI_IPC_MAX_DATA_SIZE (HPI_IPC_MAX_MSG_SIZE - sizeof(struct hpi_ipc_msg))

/* IPC Module APIs */

/**
 * @brief Initialize IPC module
 * 
 * @return 0 on success, negative error code on failure
 */
int hpi_ipc_init(void);

/**
 * @brief Send IPC message
 * 
 * @param msg_type Message type from enum hpi_ipc_msg_type
 * @param data Pointer to data to send
 * @param len Length of data
 * @return 0 on success, negative error code on failure
 */
int hpi_ipc_send(enum hpi_ipc_msg_type msg_type, const void *data, size_t len);

/**
 * @brief Check if IPC is ready for communication
 * 
 * @return true if ready, false otherwise
 */
bool hpi_ipc_is_ready(void);

/**
 * @brief Register callback for received messages
 * 
 * @param msg_type Message type to handle
 * @param callback Callback function to handle message
 * @param user_data User data passed to callback
 * @return 0 on success, negative error code on failure
 */
typedef void (*hpi_ipc_msg_callback_t)(enum hpi_ipc_msg_type msg_type,
					const void *data, size_t len,
					void *user_data);

int hpi_ipc_register_callback(enum hpi_ipc_msg_type msg_type,
			      hpi_ipc_msg_callback_t callback,
			      void *user_data);

/**
 * @brief Get IPC statistics
 */
struct hpi_ipc_stats {
	uint32_t messages_sent;
	uint32_t messages_received;
	uint32_t send_errors;
	uint32_t receive_errors;
};

void hpi_ipc_get_stats(struct hpi_ipc_stats *stats);

/**
 * @brief Initialize M4-specific IPC functionality with default callbacks
 * 
 * This function initializes the IPC service, registers default message
 * handlers for common message types, and starts the IPC thread for the M4 core.
 * 
 * @return 0 on success, negative error code on failure
 */
int hpi_ipc_m4_init(void);

#ifdef __cplusplus
}
#endif


/* M4 firmware version report (M4 -> M7), sent once when the endpoint binds;
 * surfaces as m4fw in group-64 device_info / fw_versions. Fixed 32 B and
 * NUL-terminated so a shorter string never leaks stale bytes across the IPC
 * boundary. */
#define HPI_IPC_VERSION_STR_MAX 32

struct hpi_ipc_version {
    char version[HPI_IPC_VERSION_STR_MAX];
} __packed;

#endif /* IPC_MODULE_H */
