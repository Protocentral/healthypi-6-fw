/*
 * HealthyLink Compute - Arrhythmia Classification API
 * Copyright (c) 2025-2026 ProtoCentral
 * SPDX-License-Identifier: MIT
 *
 * Multimodal arrhythmia detection using ECG + PPG signals.
 * Offloads inference to STM32N657 NPU via HealthyLink SPI.
 *
 * Based on dual-branch CNN architecture with late fusion:
 * - ECG branch: 748 samples (1.5s @ 500 Hz)
 * - PPG branch: 128 samples (1.0s @ 125 Hz)
 * - Output: 5 arrhythmia classes
 *
 * See docs/HEALTHYLINK_NPU_ARRHYTHMIA_IMPL.md for architecture details.
 */

#ifndef INCLUDE_HEALTHYLINK_HEALTHYLINK_COMPUTE_ARRHYTHMIA_H_
#define INCLUDE_HEALTHYLINK_HEALTHYLINK_COMPUTE_ARRHYTHMIA_H_

#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup healthylink_compute_arrhythmia Arrhythmia Classification
 * @{
 */

/*
 * Model Configuration Constants
 */

/** ECG input window size: 1.5 seconds @ 500 Hz */
#define ARRHYTHMIA_ECG_WINDOW_SIZE   748

/** PPG input window size: 1.0 seconds @ 125 Hz */
#define ARRHYTHMIA_PPG_WINDOW_SIZE   128

/** Number of output classes */
#define ARRHYTHMIA_NUM_CLASSES       5

/** Total input size (ECG + PPG) */
#define ARRHYTHMIA_INPUT_SIZE        (ARRHYTHMIA_ECG_WINDOW_SIZE + ARRHYTHMIA_PPG_WINDOW_SIZE)

/** Default inference timeout (milliseconds) */
#define ARRHYTHMIA_INFERENCE_TIMEOUT_MS  50

/**
 * @brief Arrhythmia classification classes
 */
enum arrhythmia_class {
	/** Normal Sinus Rhythm (60-100 BPM, regular P-QRS-T) */
	ARRHYTHMIA_NORMAL = 0,

	/** Atrial Fibrillation (irregular RR, absent P waves) */
	ARRHYTHMIA_AF = 1,

	/** Bradycardia (HR < 60 BPM) */
	ARRHYTHMIA_BRADYCARDIA = 2,

	/** Tachycardia (HR > 100 BPM) */
	ARRHYTHMIA_TACHYCARDIA = 3,

	/** Premature Ventricular Contraction (wide QRS, compensatory pause) */
	ARRHYTHMIA_PVC = 4,
};

/**
 * @brief Input structure for arrhythmia classification
 *
 * ECG and PPG data must be normalized to INT8 range [-128, 127].
 * See arrhythmia_normalize_ecg() and arrhythmia_normalize_ppg().
 */
struct healthylink_compute_arrhythmia_input {
	/** Normalized ECG samples (1.5s @ 500 Hz) */
	int8_t ecg_data[ARRHYTHMIA_ECG_WINDOW_SIZE];

	/** Normalized PPG samples (1.0s @ 125 Hz) */
	int8_t ppg_data[ARRHYTHMIA_PPG_WINDOW_SIZE];
} __packed;

/**
 * @brief Output structure from arrhythmia classification
 */
struct healthylink_compute_arrhythmia_output {
	/** Softmax output scores for each class (INT8 quantized) */
	int8_t class_scores[ARRHYTHMIA_NUM_CLASSES];

	/** Predicted class (argmax of scores) */
	uint8_t predicted_class;

	/** Confidence percentage (0-100%) */
	uint8_t confidence;

	/** Inference time in microseconds */
	uint32_t inference_time_us;
} __packed;

/**
 * @brief Arrhythmia detection result (for callbacks)
 */
struct arrhythmia_result {
	/** Predicted class */
	enum arrhythmia_class predicted_class;

	/** Confidence percentage (0-100%) */
	uint8_t confidence;

	/** Raw class scores */
	int8_t class_scores[ARRHYTHMIA_NUM_CLASSES];

	/** Timestamp when classification was performed */
	uint32_t timestamp_ms;

	/** Inference time in microseconds */
	uint32_t inference_time_us;
};

/**
 * @brief Arrhythmia detection statistics
 */
struct arrhythmia_stats {
	/** Total number of inferences performed */
	uint32_t inference_count;

	/** Number of failed inferences */
	uint32_t error_count;

	/** Whether NPU acceleration is enabled */
	bool npu_enabled;

	/** Count of each class detected */
	uint32_t class_counts[ARRHYTHMIA_NUM_CLASSES];

	/** Average inference time (microseconds) */
	uint32_t avg_inference_time_us;

	/** Maximum inference time (microseconds) */
	uint32_t max_inference_time_us;
};

/**
 * @brief Callback for arrhythmia classification results
 *
 * @note This callback is invoked from a workqueue context, not ISR.
 *       It is safe to call logging functions but avoid blocking operations.
 *
 * @param result Pointer to classification result
 * @param user_data User-provided context pointer
 */
typedef void (*arrhythmia_result_cb)(const struct arrhythmia_result *result,
				     void *user_data);

/**
 * @brief Class name strings
 */
static const char * const arrhythmia_class_names[] = {
	"Normal",
	"AF",
	"Bradycardia",
	"Tachycardia",
	"PVC"
};

/**
 * @brief Get class name from enum
 *
 * @param cls Arrhythmia class enum value
 * @return Human-readable class name string
 */
static inline const char *arrhythmia_class_name(enum arrhythmia_class cls)
{
	if (cls < ARRHYTHMIA_NUM_CLASSES) {
		return arrhythmia_class_names[cls];
	}
	return "Unknown";
}

/*
 * API Functions
 */

/**
 * @brief Check if NPU module with arrhythmia model is available
 *
 * @return true if NPU is available and arrhythmia model is loaded
 */
bool healthylink_compute_arrhythmia_available(void);

/**
 * @brief Initialize arrhythmia classification
 *
 * Initializes the NPU module and loads the arrhythmia model.
 * Must be called before any classification operations.
 *
 * @return 0 on success, negative errno on failure
 */
int healthylink_compute_arrhythmia_init(void);

/**
 * @brief Run arrhythmia classification on preprocessed input
 *
 * Sends normalized ECG+PPG data to NPU for inference.
 * Blocks until inference completes or timeout.
 *
 * @param input Pointer to preprocessed input data
 * @param output Pointer to structure for classification result
 * @param timeout Maximum time to wait for inference
 * @return 0 on success, -ETIMEDOUT on timeout, negative errno on failure
 */
int healthylink_compute_classify_arrhythmia(
	const struct healthylink_compute_arrhythmia_input *input,
	struct healthylink_compute_arrhythmia_output *output,
	k_timeout_t timeout);

/**
 * @brief Register callback for classification results
 *
 * The callback will be invoked each time a classification completes
 * when using the continuous monitoring mode.
 *
 * @param cb Callback function (NULL to unregister)
 * @param user_data User context passed to callback
 */
void healthylink_compute_arrhythmia_set_callback(arrhythmia_result_cb cb,
					    void *user_data);

/**
 * @brief Get classification statistics
 *
 * @param stats Pointer to structure for statistics
 */
void healthylink_compute_arrhythmia_get_stats(struct arrhythmia_stats *stats);

/**
 * @brief Reset classification statistics
 */
void healthylink_compute_arrhythmia_reset_stats(void);

/*
 * Normalization Helper Functions
 */

/**
 * @brief Normalize ECG sample to INT8 range
 *
 * Converts 24-bit signed ADC value to INT8 for model input.
 * Uses practical ECG range of ±500,000 (after filtering).
 *
 * @param raw_sample Raw 24-bit signed ADC value
 * @return Normalized INT8 value [-127, 127]
 */
static inline int8_t arrhythmia_normalize_ecg(int32_t raw_sample)
{
	/* ECG typical range: ±500,000 after filtering */
	const int32_t ECG_SCALE = 500000;

	/* Clamp to expected range */
	int32_t clamped = raw_sample;
	if (clamped > ECG_SCALE) {
		clamped = ECG_SCALE;
	} else if (clamped < -ECG_SCALE) {
		clamped = -ECG_SCALE;
	}

	/* Scale to [-127, 127] */
	return (int8_t)((clamped * 127) / ECG_SCALE);
}

/**
 * @brief Normalize PPG sample to INT8 range
 *
 * Converts PPG ADC value to INT8 relative to baseline.
 * PPG is AC-coupled: output represents deviation from DC baseline.
 *
 * @param raw_sample Raw PPG ADC value
 * @param baseline DC baseline (moving average)
 * @param amplitude Peak-to-peak amplitude / 2
 * @return Normalized INT8 value [-127, 127]
 */
static inline int8_t arrhythmia_normalize_ppg(uint32_t raw_sample,
					      uint32_t baseline,
					      uint32_t amplitude)
{
	/* AC component relative to baseline */
	int32_t ac_component = (int32_t)raw_sample - (int32_t)baseline;

	/* Avoid division by zero */
	if (amplitude == 0) {
		amplitude = 1;
	}

	/* Scale based on measured amplitude */
	int32_t normalized = (ac_component * 127) / (int32_t)amplitude;

	/* Clamp to INT8 range */
	if (normalized > 127) {
		return 127;
	} else if (normalized < -127) {
		return -127;
	}
	return (int8_t)normalized;
}

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_HEALTHYLINK_HEALTHYLINK_COMPUTE_ARRHYTHMIA_H_ */
