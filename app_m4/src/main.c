/*
 * Copyright (c) 2024-2026 Protocentral Electronics
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "ipc_module.h"
#include "algorithm_module.h"
#include "spo2_module.h"
#include "../../app_m7/src/hpi_common_types.h"

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
LOG_MODULE_REGISTER(healthypi6_m4, LOG_LEVEL_INF);

/* Workqueue pattern (load-bearing): IPC callbacks must only enqueue and
 * return in <100 µs — any processing in the callback blocks the IPC thread
 * and overflows the RPMSG buffers. Work handlers drain the message queues
 * asynchronously; the queues provide the elastic buffering.
 */

/* Message queues buffer data between fast IPC callbacks and slower processing */
K_MSGQ_DEFINE(q_ppg_batches, 
              sizeof(struct hpi_ipc_ppg_raw_batch), 
              32,  /* 32 entries = ~256ms buffering at 125 batches/sec */
              4);  /* Alignment */

K_MSGQ_DEFINE(q_ecg_batches, 
              sizeof(struct hpi_ipc_ecg_raw_batch), 
              32,  /* 32 entries = ~1s buffering at 31.25 batches/sec */
              4);

K_MSGQ_DEFINE(q_eeg_batches, 
              sizeof(struct hpi_ipc_eeg_raw_batch), 
              32,  /* 32 entries = ~1s buffering at 31.25 batches/sec */
              4);

/* ============================================================================
 * Statistics Tracking
 * ============================================================================ */

/* PPG Statistics */
static uint32_t ppg_batches_received = 0;
static uint32_t ppg_samples_received = 0;
static uint32_t ppg_last_sample_num = 0;
static uint32_t ppg_sequence_errors = 0;
static uint32_t ppg_last_timestamp = 0;
static atomic_t ppg_queue_drops = ATOMIC_INIT(0);

/* ECG Statistics */
static uint32_t ecg_batches_received = 0;
static uint32_t ecg_samples_received = 0;
static uint32_t ecg_last_sample_num = 0;
static uint32_t ecg_sequence_errors = 0;
static uint32_t ecg_last_timestamp = 0;
static atomic_t ecg_queue_drops = ATOMIC_INIT(0);

/* EEG Statistics */
static uint32_t eeg_batches_received = 0;
static uint32_t eeg_samples_received = 0;
static uint32_t eeg_last_sample_num = 0;
static uint32_t eeg_sequence_errors = 0;
static uint32_t eeg_last_timestamp = 0;
static atomic_t eeg_queue_drops = ATOMIC_INIT(0);

/* ============================================================================
 * Work Handlers - Forward Declarations
 * ============================================================================ */

static void ppg_work_handler(struct k_work *work);
static void ecg_work_handler(struct k_work *work);
static void eeg_work_handler(struct k_work *work);

/* Work items (triggered by IPC callbacks) */
K_WORK_DEFINE(ppg_work, ppg_work_handler);
K_WORK_DEFINE(ecg_work, ecg_work_handler);
K_WORK_DEFINE(eeg_work, eeg_work_handler);

/* IPC callbacks - fast enqueue only. They run in IPC thread context
 * (priority 5) and MUST return in <100 µs: validate size, k_msgq_put with
 * K_NO_WAIT, submit the work item, return. All processing happens in the
 * work handlers. */

/**
 * @brief PPG IPC callback - enqueue for ppg_work_handler()
 *
 * @param type Message type (HPI_IPC_MSG_TYPE_PPG_RAW = 0x10)
 * @param data Pointer to hpi_ipc_ppg_raw_batch structure
 * @param len Length in bytes (should be 388 bytes)
 * @param user_data User data (unused)
 */
static void ppg_raw_callback(enum hpi_ipc_msg_type type, 
                            const void *data, 
                            size_t len, 
                            void *user_data)
{
	/* Quick validation - reject obviously invalid data */
	if (len < sizeof(struct hpi_ipc_ppg_raw_batch)) {
		return;  /* Silently drop - avoid logging in hot path */
	}
	
	/* Fast path: enqueue data for processing */
	if (k_msgq_put(&q_ppg_batches, data, K_NO_WAIT) == 0) {
		/* Successfully queued - trigger work handler */
		k_work_submit(&ppg_work);
	} else {
		/* Queue full - drop message (backpressure) */
		atomic_inc(&ppg_queue_drops);
	}
}

/**
 * @brief ECG IPC callback - enqueue for ecg_work_handler()
 *
 * @param msg_type Message type (HPI_IPC_MSG_TYPE_ECG_RAW = 0x20)
 * @param data Pointer to hpi_ipc_ecg_raw_batch structure
 * @param len Length in bytes (should be 452 bytes)
 * @param user_data User data (unused)
 */
static void ecg_raw_callback(enum hpi_ipc_msg_type msg_type, 
                             const void *data, size_t len, void *user_data)
{
	/* Quick validation */
	if (len < sizeof(struct hpi_ipc_ecg_raw_batch)) {
		return;
	}
	
	/* Fast path: enqueue */
	if (k_msgq_put(&q_ecg_batches, data, K_NO_WAIT) == 0) {
		k_work_submit(&ecg_work);
	} else {
		atomic_inc(&ecg_queue_drops);
	}
}

/**
 * @brief EEG IPC callback - enqueue for eeg_work_handler()
 *
 * @param msg_type Message type (HPI_IPC_MSG_TYPE_EEG_RAW = 0x30)
 * @param data Pointer to hpi_ipc_eeg_raw_batch structure
 * @param len Length in bytes (should be 324 bytes)
 * @param user_data User data (unused)
 */
static void eeg_raw_callback(enum hpi_ipc_msg_type msg_type, 
                             const void *data, size_t len, void *user_data)
{
	/* Quick validation */
	if (len < sizeof(struct hpi_ipc_eeg_raw_batch)) {
		return;
	}
	
	/* Fast path: enqueue */
	if (k_msgq_put(&q_eeg_batches, data, K_NO_WAIT) == 0) {
		k_work_submit(&eeg_work);
	} else {
		atomic_inc(&eeg_queue_drops);
	}
}

/* Work handlers - run in system work queue context at lower priority than
 * the IPC thread. They drain the message queues and do the real processing
 * (validation, stats, sequence checks, algorithms) and may take multiple
 * milliseconds without blocking IPC reception. */

/**
 * @brief PPG work handler - drain and process queued PPG batches
 *
 * @param work Work item (ppg_work)
 */
static void ppg_work_handler(struct k_work *work)
{
	struct hpi_ipc_ppg_raw_batch batch;
	
	/* Process all queued batches (drain queue) */
	while (k_msgq_get(&q_ppg_batches, &batch, K_NO_WAIT) == 0) {
		
		/* Validation (moved from callback) */
		if (batch.sample_count == 0 || batch.sample_count > HPI_PPG_BATCH_SIZE) {
			LOG_WRN("Invalid PPG sample_count: %u", batch.sample_count);
			continue;
		}
		
		/* Update statistics (moved from callback) */
		ppg_batches_received++;
		ppg_samples_received += batch.sample_count;
		
		/* Check for sequence errors (moved from callback) */
		if (ppg_batches_received > 1) {
			uint32_t first_sample_num = batch.samples[0].sample_number;
			uint32_t expected_num = (ppg_last_sample_num + 1) % 65536;
			
			if (first_sample_num != expected_num) {
				ppg_sequence_errors++;
				LOG_WRN("PPG sequence error: expected %u, got %u (gap: %d)",
				        expected_num, first_sample_num,
				        (int)(first_sample_num - expected_num));
			}
		}
		
		/* Update last sample tracking */
		ppg_last_sample_num = batch.samples[batch.sample_count - 1].sample_number;
		ppg_last_timestamp = batch.samples[batch.sample_count - 1].timestamp_ms;
		
		/* Periodic debug log to verify M4 receive integrity */
		if ((ppg_batches_received % 100) == 0) {
			LOG_DBG("[M4-RX] Batch %u: IR[0]=%d, Red[0]=%d",
			        ppg_batches_received,
			        batch.samples[0].ir_raw,
			        batch.samples[0].red_raw);
		}

		/* The M4 deliberately sends no per-batch acknowledgment: the
		 * bidirectional traffic saturates the IPC buffers even from work
		 * context. The same applies to the ECG and EEG handlers below. */

		/* TODO: Algorithm processing
		 * spo2_algorithm_process_batch(&algo_state, &batch);
		 */
	}
}

/**
 * @brief ECG work handler - drain and process queued ECG batches
 *
 * @param work Work item (ecg_work)
 */
static void ecg_work_handler(struct k_work *work)
{
	struct hpi_ipc_ecg_raw_batch batch;
	
	/* Process all queued batches */
	while (k_msgq_get(&q_ecg_batches, &batch, K_NO_WAIT) == 0) {
		
		/* Validation */
		if (batch.sample_count == 0 || batch.sample_count > HPI_ECG_BATCH_SIZE) {
			LOG_WRN("Invalid ECG sample_count: %u", batch.sample_count);
			continue;
		}
		
		/* Update statistics */
		ecg_batches_received++;
		ecg_samples_received += batch.sample_count;
		
		/* Check for sequence gaps */
		if (ecg_batches_received > 1) {
			uint32_t expected_num = ecg_last_sample_num + 1;
			uint32_t actual_num = batch.samples[0].sample_number;
			if (actual_num != expected_num) {
				ecg_sequence_errors++;
				LOG_WRN("ECG sequence gap: expected %u, got %u (gap=%d)",
				        expected_num, actual_num, (int32_t)(actual_num - expected_num));
			}
		}
		
		/* Update last sample tracking */
		ecg_last_sample_num = batch.samples[batch.sample_count - 1].sample_number;
		ecg_last_timestamp = batch.samples[batch.sample_count - 1].timestamp_ms;

		/* TODO: QRS detection algorithm
		 * pan_tompkins_process_batch(&qrs_state, &batch);
		 */
	}
}

/**
 * @brief EEG work handler - drain and process queued EEG batches
 *
 * @param work Work item (eeg_work)
 */
static void eeg_work_handler(struct k_work *work)
{
	struct hpi_ipc_eeg_raw_batch batch;
	
	/* Process all queued batches */
	while (k_msgq_get(&q_eeg_batches, &batch, K_NO_WAIT) == 0) {
		
		/* Validation */
		if (batch.sample_count == 0 || batch.sample_count > HPI_EEG_BATCH_SIZE) {
			LOG_WRN("Invalid EEG sample_count: %u", batch.sample_count);
			continue;
		}
		
		/* Update statistics */
		eeg_batches_received++;
		eeg_samples_received += batch.sample_count;
		
		/* Check for sequence gaps */
		if (eeg_batches_received > 1) {
			uint32_t expected_num = eeg_last_sample_num + 1;
			uint32_t actual_num = batch.samples[0].sample_number;
			if (actual_num != expected_num) {
				eeg_sequence_errors++;
				LOG_WRN("EEG sequence gap: expected %u, got %u (gap=%d)",
				        expected_num, actual_num, (int32_t)(actual_num - expected_num));
			}
		}
		
		/* Update last sample tracking */
		eeg_last_sample_num = batch.samples[batch.sample_count - 1].sample_number;
		eeg_last_timestamp = batch.samples[batch.sample_count - 1].timestamp_ms;

		/* TODO: EEG frequency band analysis
		 * eeg_fft_process_batch(&eeg_state, &batch);
		 */
	}
}

/* Statistics printing */

/**
 * @brief Print PPG reception statistics
 */
static void print_ppg_stats(void)
{
	LOG_INF("========================================");
	LOG_INF("PPG Reception Statistics (M4)");
	LOG_INF("========================================");
	LOG_INF("  Batches received:    %u", ppg_batches_received);
	LOG_INF("  Samples received:    %u", ppg_samples_received);
	LOG_INF("  Sequence errors:     %u", ppg_sequence_errors);
	LOG_INF("  Last sample #:       %u", ppg_last_sample_num);
	LOG_INF("  Last timestamp:      %u ms", ppg_last_timestamp);
	
	if (ppg_batches_received > 0) {
		uint32_t uptime_sec = k_uptime_get_32() / 1000;
		if (uptime_sec > 0) {
			uint32_t batch_rate = ppg_batches_received / uptime_sec;
			uint32_t sample_rate = ppg_samples_received / uptime_sec;
			LOG_INF("  Batch rate:          %u batches/sec (target: 31)", batch_rate);
			LOG_INF("  Sample rate:         %u samples/sec (target: 500)", sample_rate);
			
			if (ppg_sequence_errors > 0) {
				uint32_t error_pct = (ppg_sequence_errors * 100) / ppg_batches_received;
				LOG_INF("  Error rate:          %u%% (%u errors)", 
				        error_pct, ppg_sequence_errors);
			}
		}
	}
	LOG_INF("========================================");
}

/**
 * @brief Print ECG reception statistics
 */
static void print_ecg_stats(void)
{
	LOG_INF("========================================");
	LOG_INF("ECG Reception Statistics (M4)");
	LOG_INF("========================================");
	LOG_INF("  Batches received:    %u", ecg_batches_received);
	LOG_INF("  Samples received:    %u", ecg_samples_received);
	LOG_INF("  Sequence errors:     %u", ecg_sequence_errors);
	LOG_INF("  Last sample #:       %u", ecg_last_sample_num);
	LOG_INF("  Last timestamp:      %u ms", ecg_last_timestamp);
	
	if (ecg_batches_received > 0) {
		uint32_t uptime_sec = k_uptime_get_32() / 1000;
		if (uptime_sec > 0) {
			uint32_t batch_rate = ecg_batches_received / uptime_sec;
			uint32_t sample_rate = ecg_samples_received / uptime_sec;
			LOG_INF("  Batch rate:          %u batches/sec (target: 31)", batch_rate);
			LOG_INF("  Sample rate:         %u samples/sec (target: 500)", sample_rate);
			
			if (ecg_sequence_errors > 0) {
				uint32_t error_pct = (ecg_sequence_errors * 100) / ecg_batches_received;
				LOG_INF("  Error rate:          %u%% (%u errors)", 
				        error_pct, ecg_sequence_errors);
			}
		}
	}
	LOG_INF("========================================");
}

/**
 * @brief Print EEG reception statistics
 */
static void print_eeg_stats(void)
{
	LOG_INF("========================================");
	LOG_INF("EEG Reception Statistics (M4)");
	LOG_INF("========================================");
	LOG_INF("  Batches received:    %u", eeg_batches_received);
	LOG_INF("  Samples received:    %u", eeg_samples_received);
	LOG_INF("  Sequence errors:     %u", eeg_sequence_errors);
	LOG_INF("  Last sample #:       %u", eeg_last_sample_num);
	LOG_INF("  Last timestamp:      %u ms", eeg_last_timestamp);
	
	if (eeg_batches_received > 0) {
		uint32_t uptime_sec = k_uptime_get_32() / 1000;
		if (uptime_sec > 0) {
			uint32_t batch_rate = eeg_batches_received / uptime_sec;
			uint32_t sample_rate = eeg_samples_received / uptime_sec;
			LOG_INF("  Batch rate:          %u batches/sec (target: 31)", batch_rate);
			LOG_INF("  Sample rate:         %u samples/sec (target: 500)", sample_rate);
			
			if (eeg_sequence_errors > 0) {
				uint32_t error_pct = (eeg_sequence_errors * 100) / eeg_batches_received;
				LOG_INF("  Error rate:          %u%% (%u errors)", 
				        error_pct, eeg_sequence_errors);
			}
		}
	}
	LOG_INF("========================================");
}

/* ============================================================================
 * Main Function - Initialization and Statistics Loop
 * ============================================================================ */

int main(void)
{
	LOG_INF("========================================");
	LOG_INF("  HealthyPi 6 - M4 Core Starting");
	LOG_INF("  OpenAMP/RPMSG IPC Backend");
	LOG_INF("========================================");
	LOG_INF("Configuration:");
	LOG_INF("  IPC Backend:    OpenAMP Static VRINGs");
	LOG_INF("  M4 Heap Size:   %d bytes", CONFIG_HEAP_MEM_POOL_SIZE);
	LOG_INF("  PPG Queue Size: 32 entries (~256ms buffering)");
	LOG_INF("  ECG Queue Size: 32 entries (~1s buffering)");
	LOG_INF("  EEG Queue Size: 32 entries (~1s buffering)");
	LOG_INF("========================================");
	
	/* Initialize IPC for M7 communication: opens the OpenAMP instance and
	 * registers the RPMSG endpoint that binds to the M7 (M4 = REMOTE). */
	int ret = hpi_ipc_init();
	if (ret != 0) {
		LOG_ERR("M4: Failed to initialize IPC: %d", ret);
	} else {
		LOG_INF("M4: IPC initialized");
	}
	
	/* Register fast callbacks (workqueue pattern). PPG is registered by the
	 * SpO2 module and ECG by algorithm_module; only EEG is registered here,
	 * because it has no algorithm module yet. */
	ret = hpi_ipc_register_callback(HPI_IPC_MSG_TYPE_EEG_RAW,
	                                 eeg_raw_callback,
	                                 NULL);
	if (ret < 0) {
		LOG_ERR("Failed to register EEG callback: %d", ret);
	} else {
		LOG_INF("EEG callback registered (fast enqueue, Type 0x30)");
	}
	
	LOG_INF("========================================");
	LOG_INF("M4 initialization complete");
	LOG_INF("Ready to receive PPG, ECG & EEG batches");
	LOG_INF("NOTE: PPG → SpO2 module, ECG → Algorithm module, EEG → main.c");
	LOG_INF("========================================");
	
	/* Initialize Algorithm Module (QRS detection) */
	LOG_INF("Initializing algorithm module...");
	ret = algorithm_module_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize algorithm module: %d", ret);
	} else {
		LOG_INF("Algorithm module initialized successfully");
		
		/* Register algorithm IPC callbacks */
		ret = algorithm_register_ipc_callbacks();
		if (ret < 0) {
			LOG_ERR("Failed to register algorithm callbacks: %d", ret);
		} else {
			LOG_INF("Algorithm callbacks registered");
			
			/* Start algorithm processing */
			ret = algorithm_module_start();
			if (ret < 0) {
				LOG_ERR("Failed to start algorithm module: %d", ret);
			} else {
				LOG_INF("Algorithm module started - QRS detection active");
			}
		}
	}
	LOG_INF("========================================");

	/* Initialize SpO2 Module (SpO2 + PPG-HR). The M7 feeds PPG_RAW decimated
	 * to the 125 Hz this module expects (see app_m7 platform/ipc.c forward_ppg).
	 */
#if 1
	LOG_INF("Initializing SpO2 module...");
	ret = spo2_module_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize SpO2 module: %d", ret);
	} else {
		LOG_INF("SpO2 module initialized successfully");

		/* Start SpO2 processing */
		ret = spo2_module_start();
		if (ret < 0) {
			LOG_ERR("Failed to start SpO2 module: %d", ret);
		} else {
			LOG_INF("SpO2 module started - SpO2 + PPG-HR calculation active");
		}
	}
#else
	LOG_INF("SpO2 module DISABLED - re-enable when validated");
#endif
	LOG_INF("========================================");

	/* Arrhythmia / beat classification is NOT performed on the M4 core.
	 * The M4 does signal calculations only (QRS/HR/HRV, SpO2). ML arrhythmia
	 * classification runs on the HealthyLink Compute (STM32N657 NPU) module
	 * when installed - its firmware is the separate
	 * Protocentral/healthylink-compute-fw repository. */
	LOG_INF("Arrhythmia classification: handled by HealthyLink Compute module");
	LOG_INF("========================================");

	/* Main loop - minimal logging, only show warnings/errors */
	uint32_t loop_count = 0;

	for (;;) {
		k_sleep(K_SECONDS(10));
		loop_count++;

		/* Get queue usage (32 - free = used) */
		uint32_t ppg_used = 32 - k_msgq_num_free_get(&q_ppg_batches);
		uint32_t ecg_used = 32 - k_msgq_num_free_get(&q_ecg_batches);
		uint32_t eeg_used = 32 - k_msgq_num_free_get(&q_eeg_batches);

		/* Get drop counts */
		uint32_t ppg_drops = atomic_get(&ppg_queue_drops);
		uint32_t ecg_drops = atomic_get(&ecg_queue_drops);
		uint32_t eeg_drops = atomic_get(&eeg_queue_drops);

		/* Get algorithm statistics */
		struct algorithm_stats algo_stats;
		algorithm_get_stats(&algo_stats);

		/* Log warnings and errors only. */
		if (ppg_used > 16 || ecg_used > 16 || eeg_used > 16) {
			LOG_WRN("Queue usage >50%%: PPG=%u, ECG=%u, EEG=%u",
			        ppg_used, ecg_used, eeg_used);
		}
		if (ppg_drops > 0 || ecg_drops > 0 || eeg_drops > 0) {
			LOG_ERR("Message drops: PPG=%u, ECG=%u, EEG=%u",
			        ppg_drops, ecg_drops, eeg_drops);
		}
		if (algo_stats.algorithm_overruns > 0) {
			LOG_WRN("Algorithm queue overruns: %u", algo_stats.algorithm_overruns);
		}
	}

	return 0;
}
