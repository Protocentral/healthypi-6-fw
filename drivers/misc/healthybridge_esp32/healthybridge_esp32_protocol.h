/*
 * Copyright (c) 2025 Protocentral
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file healthybridge_esp32_protocol.h
 * @brief HealthyBridge wire contract -- frame layout, type IDs, payloads.
 *
 * Transport-independent by construction: the same frames cross the M7 <-> C6
 * UART link that the retired SPI link carried, and the ESP side's healthybridge.h
 * / healthybridge_hp6.h must agree byte for byte. A number that disagrees is
 * silently mis-decoded on the wire, not rejected.
 */

#ifndef HEALTHYBRIDGE_ESP32_SPI_PROTOCOL_H
#define HEALTHYBRIDGE_ESP32_SPI_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Frame Format:
 * ┌─────────┬─────────┬─────────┬─────────┬─────────┬─────────┬─────────┐
 * │  SYNC   │  TYPE   │  FLAGS  │ LENGTH  │   SEQ   │ PAYLOAD │  CRC16  │
 * │  (2B)   │  (1B)   │  (1B)   │  (2B)   │  (2B)   │  (var)  │  (2B)   │
 * └─────────┴─────────┴─────────┴─────────┴─────────┴─────────┴─────────┘
 *   0xAA55    Type      Flags     Bytes     Seq#      Data     CRC-CCITT
 */

/* Frame synchronisation marker */
#define HPI_HB_SYNC_WORD           0xAA55

/* Frame size limits */
#define HPI_HB_HEADER_SIZE         8       /* Sync(2)+Type(1)+Flags(1)+Length(2)+Seq(2) -- matches the packed header struct and the ESP HealthyBridge codec */
#define HPI_HB_CRC_SIZE            2       /* CRC-16 */
#define HPI_HB_MAX_FRAME_SIZE      4096    /* Total on-wire frame: header + payload + CRC */
#define HPI_HB_MAX_PAYLOAD_SIZE    (HPI_HB_MAX_FRAME_SIZE - HPI_HB_HEADER_SIZE - HPI_HB_CRC_SIZE)

/*
 * Message Types — MUST match the external HealthyBridge repo's HB_TYPE_* table
 * (healthybridge.h / healthybridge_hp6.h). That repo is the source of truth for
 * the ESP32 side; a number that disagrees here is silently mis-decoded on the
 * wire, not rejected.
 */
#define HPI_HB_MSG_TYPE_ECG_DATA       0x20    /* ECG sample batch    (= HB_TYPE_BIOSIG) */
#define HPI_HB_MSG_TYPE_PPG_DATA       0x10    /* PPG batch red+IR    (= HB_TYPE_PPG) */
#define HPI_HB_MSG_TYPE_RESP_DATA      0x30    /* Respiration data    (= HB_TYPE_RESP) */
#define HPI_HB_MSG_TYPE_VITALS         0x40    /* Computed vitals     (= HB_TYPE_VITALS) */
#define HPI_HB_MSG_TYPE_BATTERY        0x41    /* Battery SoC/mV      (= HB_TYPE_BATTERY) */
#define HPI_HB_MSG_TYPE_HRV            0x42    /* HRV metrics         (= HB_TYPE_HRV)
                                                 * NOT 0x41: the ESP decodes 0x41 as a
                                                 * battery payload. */
#define HPI_HB_MSG_TYPE_CONTROL_CMD    0x50    /* Control command (M7 -> ESP32) */
#define HPI_HB_MSG_TYPE_CONTROL_RESP   0x51    /* Control response (ESP32 → M7) */
#define HPI_HB_MSG_TYPE_STATUS_REQ     0x60    /* Status request (M7 → ESP32) */
#define HPI_HB_MSG_TYPE_STATUS_RESP    0x61    /* Status response (ESP32 → M7) */

/* Frame Flags (bitfield) */
#define HPI_HB_FLAG_COMPRESSED     (1 << 0)    /* Payload is compressed */
#define HPI_HB_FLAG_ENCRYPTED      (1 << 1)    /* Payload is encrypted */
#define HPI_HB_FLAG_LAST           (1 << 2)    /* Last frame in batch */
#define HPI_HB_FLAG_ACK_REQ        (1 << 3)    /* Requires acknowledgment */
#define HPI_HB_FLAG_ERROR          (1 << 7)    /* Error indicator */

/**
 * @brief SPI frame header structure (10 bytes)
 * 
 * Always transmitted in little-endian format.
 */
struct hpi_hb_frame_header {
    uint16_t sync;          /* Sync word: 0xAA55 */
    uint8_t type;           /* Message type (HPI_HB_MSG_TYPE_*) */
    uint8_t flags;          /* Control flags (HPI_HB_FLAG_*) */
    uint16_t length;        /* Payload length in bytes (little-endian) */
    uint16_t seq;           /* Sequence number (little-endian) */
} __attribute__((packed));

/**
 * @brief Complete SPI frame structure
 * 
 * Variable-length frame with header, payload, and CRC.
 */
struct hpi_hb_frame {
    struct hpi_hb_frame_header header;
    uint8_t payload[HPI_HB_MAX_PAYLOAD_SIZE];
    /* CRC16 appended after payload (not in struct due to variable length) */
} __attribute__((packed));

/*
 * Payload Structures
 */

/**
 * @brief Single ECG sample with all channels (ECG + ADC + PPG)
 *
 * Extended format includes ADC and PPG channels for unified USB/WiFi streaming.
 * PPG is included per-sample to avoid staircase effect from batch caching.
 * Total: 32 bytes per sample (8 x int32_t)
 */
struct hpi_ecg_sample_multi {
    int32_t ch0;      /* Respiration (Lead II - Lead I) */
    int32_t ch1;      /* ECG Lead I */
    int32_t ch2;      /* ECG Lead II */
    int32_t ch3;      /* ECG Lead III */
    int32_t adc_ch1;  /* ADC Channel 1 (INP14/PA2) */
    int32_t adc_ch2;  /* ADC Channel 2 (INP15/PA3) */
    int32_t ppg_red;  /* PPG Red LED (per-sample to avoid staircase) */
    int32_t ppg_ir;   /* PPG IR LED (per-sample to avoid staircase) */
} __attribute__((packed));

/**
 * @brief ECG data payload (TYPE = 0x20)
 *
 * Batch of ECG samples captured at a fixed sampling rate.
 * Single-channel version (legacy, used when only ch1 is batched)
 */
struct hpi_hb_ecg_payload {
    uint32_t timestamp_ms;      /* M7 uptime when first sample captured */
    uint16_t sample_count;      /* Number of samples in this frame */
    uint16_t sample_rate_hz;    /* Sampling rate (e.g., 500 Hz) */
    int32_t samples[];          /* ECG samples (24-bit sign-extended to 32-bit) */
} __attribute__((packed));

/**
 * @brief ECG multi-channel data payload (TYPE = 0x20)
 *
 * Batch of multi-channel ECG samples (all 4 channels per sample).
 * This is the PREFERRED format for complete biosignal data.
 */
struct hpi_hb_ecg_payload_multi {
    uint32_t timestamp_ms;      /* M7 uptime when first sample captured */
    uint16_t sample_count;      /* Number of samples (each with 4 channels) */
    uint16_t sample_rate_hz;    /* Sampling rate (e.g., 500 Hz) */
    struct hpi_ecg_sample_multi samples[];  /* Multi-channel samples */
} __attribute__((packed));

/**
 * @brief PPG sample pair (red + IR channels)
 */
struct hpi_ppg_sample {
    int32_t red;                /* Red LED channel value */
    int32_t ir;                 /* IR LED channel value */
} __attribute__((packed));

/**
 * @brief PPG data payload (TYPE = 0x10)
 * 
 * Batch of PPG sample pairs (red + IR channels).
 */
struct hpi_hb_ppg_payload {
    uint32_t timestamp_ms;      /* M7 uptime when first sample captured */
    uint16_t sample_count;      /* Number of sample pairs */
    uint16_t sample_rate_hz;    /* Sampling rate (e.g., 125 Hz) */
    struct hpi_ppg_sample samples[];  /* PPG sample pairs */
} __attribute__((packed));

/**
 * @brief Vitals payload (TYPE = 0x40)
 *
 * Computed vital signs transmitted at low rate (~1 Hz).
 */
struct hpi_hb_vitals_payload {
    uint32_t timestamp_ms;      /* M7 uptime */
    uint8_t heart_rate_bpm;     /* Heart rate (0-255 BPM) */
    uint8_t spo2_percent;       /* SpO2 (0-100 %) */
    uint8_t resp_rate_bpm;      /* Respiration rate (0-255 breaths/min) */
    uint8_t reserved;
    int16_t temp_celsius_x10;   /* Temperature * 10 (e.g., 372 = 37.2°C) */
    uint16_t status_flags;      /* Validity flags for each vital */
} __attribute__((packed));

/**
 * @brief HRV payload (TYPE = 0x41)
 *
 * Heart Rate Variability metrics transmitted at low rate (~0.2 Hz / every 5s).
 * Calculated from 64 RR intervals on M4 core.
 */
struct hpi_hb_hrv_payload {
    uint32_t timestamp_ms;      /* M7 uptime when calculated */
    uint16_t heart_rate_bpm;    /* Heart rate (0-300 BPM) */
    uint16_t rr_interval_ms;    /* Latest RR interval in ms */
    uint16_t hrv_sdnn_ms;       /* SDNN (Std Dev of NN intervals) in ms */
    uint16_t hrv_rmssd_ms;      /* RMSSD (Root Mean Square of Successive Diff) in ms */
    uint8_t hrv_pnn50;          /* pNN50 percentage (0-100) */
    uint8_t signal_quality;     /* ECG signal quality (0-100%) */
    uint8_t hrv_valid;          /* 1 if HRV data is valid, 0 if learning */
    uint8_t arrhythmia_flags;   /* Arrhythmia detection flags */
    uint16_t mean_rr_ms;        /* Mean RR interval in ms */
    uint16_t reserved;          /* Future use */
} __attribute__((packed));

/* HRV Arrhythmia Flags (matches hpi_common_types.h) */
#define HPI_HRV_ARRHYTHMIA_NONE         0x00
#define HPI_HRV_ARRHYTHMIA_BRADYCARDIA  (1 << 3)    /* HR < 60 BPM */
#define HPI_HRV_ARRHYTHMIA_TACHYCARDIA  (1 << 4)    /* HR > 100 BPM */

/**
 * @brief Control command payload (TYPE = 0x50)
 * 
 * Commands sent from M7 to ESP32 for control operations.
 */
struct hpi_hb_control_cmd {
    uint8_t cmd_id;             /* Command identifier */
    uint8_t reserved;
    uint16_t param_len;         /* Parameter length */
    uint8_t params[];           /* Command-specific parameters */
} __attribute__((packed));

/*
 * Control Command IDs — MUST match the external HealthyBridge repo's HB_CMD_*
 * table (healthybridge.h). Only the commands below have a handler in that
 * repo's control.c; anything else hits its `default:` and is silently ignored.
 * A wrong number is worse than inert — it can alias onto a different handler
 * (e.g. a status poll re-enabling the radio).
 */
#define HPI_HB_CMD_PING                 0x01  /* = HB_CMD_PING */
#define HPI_HB_CMD_BLE_ADV_START        0x10  /* = HB_CMD_BLE_ADV_START */
#define HPI_HB_CMD_BLE_ADV_STOP         0x11  /* = HB_CMD_BLE_ADV_STOP */
#define HPI_HB_CMD_BLE_SET_NAME         0x12  /* = HB_CMD_BLE_SET_NAME (payload: name) */
#define HPI_HB_CMD_WIFI_ENABLE          0x20  /* = HB_CMD_WIFI_ENABLE  (STA, stored creds) */
#define HPI_HB_CMD_WIFI_DISABLE         0x21  /* = HB_CMD_WIFI_DISABLE (stop STA / AP) */
#define HPI_HB_CMD_WIFI_SOFTAP          0x22  /* = HB_CMD_WIFI_SOFTAP  (captive portal) */
#define HPI_HB_CMD_GET_STATUS           0x30  /* = HB_CMD_GET_STATUS -> HB_TYPE_STATUS reply */

/*
 * Any command not listed above is NOT IMPLEMENTED by the external ESP firmware
 * and is deliberately left undefined here, so a caller cannot send a command
 * the ESP will ignore — or worse, alias onto a different handler. WiFi
 * credentials are provisioned through the SoftAP captive portal
 * (HPI_HB_CMD_WIFI_SOFTAP), not pushed over the link.
 */

/* Response status codes */
#define HPI_HB_RESP_OK                  0x00
#define HPI_HB_RESP_ERROR               0x01
#define HPI_HB_RESP_PENDING             0x02
#define HPI_HB_RESP_NOT_SUPPORTED       0x03
#define HPI_HB_RESP_BUSY                0x04

/* WiFi state values (in response data) */
#define HPI_WIFI_STATE_DISCONNECTED      0x00
#define HPI_WIFI_STATE_CONNECTING        0x01
#define HPI_WIFI_STATE_CONNECTED         0x02
#define HPI_WIFI_STATE_AP_MODE           0x03
#define HPI_WIFI_STATE_ERROR             0xFF

/* MQTT state values (in response data) */
#define HPI_MQTT_STATE_DISCONNECTED      0x00
#define HPI_MQTT_STATE_CONNECTING        0x01
#define HPI_MQTT_STATE_CONNECTED         0x02
#define HPI_MQTT_STATE_ERROR             0xFF

/**
 * @brief WiFi connect parameters (for HPI_HB_CMD_WIFI_CONNECT)
 */
struct hpi_hb_wifi_connect_params {
    uint8_t ssid_len;
    uint8_t password_len;
    char ssid[32];
    char password[64];
} __attribute__((packed));

/**
 * @brief Link status: WiFi association plus BLE advertising/connection state.
 *
 * Carried two ways by the co-processor, and the shapes must match:
 *   - as the data[] of a CONTROL_RESP answering GET_STATUS (0x30);
 *   - as the payload of an unsolicited STATUS_RESP (0x61) at 1 Hz, which is
 *     what lets the driver answer wifi_status() from cache.
 *
 * Must equal the ESP side's `struct hb_wifi_status_resp_hp6` byte for byte;
 * both carry a _Static_assert on the size. Fields must only ever be APPENDED
 * so version skew degrades rather than corrupting: a short frame fails the
 * decoder's `len >=` check, and an older host copies MIN(data_len, out_cap)
 * and ignores the tail.
 */
struct hpi_hb_wifi_status_resp {
    uint8_t state;              /* HPI_WIFI_STATE_* */
    int8_t rssi;                /* Signal strength (dBm), valid when connected */
    uint8_t ip_addr[4];         /* IP address, valid when connected */
    char ssid[32];              /* Connected SSID, null-terminated */
    uint8_t ble_adv;            /* 1 = advertising */
    uint8_t ble_conn;           /* 1 = a central is connected */
} __attribute__((packed));

/**
 * @brief MQTT status response data
 */
struct hpi_hb_mqtt_status_resp {
    uint8_t state;              /* HPI_MQTT_STATE_* */
    uint8_t reserved[3];
} __attribute__((packed));

/**
 * @brief Version response data
 */
struct hpi_hb_version_resp {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t reserved;
    char version_string[32];    /* Human-readable version */
} __attribute__((packed));

/**
 * @brief Control response payload (TYPE = 0x51)
 * 
 * Response from ESP32 to M7 control commands.
 */
struct hpi_hb_control_resp {
    uint8_t cmd_id;             /* Echo of command ID */
    uint8_t status;             /* 0 = success, non-zero = error code */
    uint16_t data_len;          /* Response data length */
    uint8_t data[];             /* Response data (optional) */
} __attribute__((packed));

/**
 * @brief Status request payload (TYPE = 0x60)
 * 
 * Query ESP32 for current status.
 */
struct hpi_hb_status_req {
    uint8_t query_mask;         /* Bitfield: WiFi(0), BLE(1), MQTT(2), Buffer(3) */
} __attribute__((packed));

/**
 * @brief Status response payload (TYPE = 0x61)
 * 
 * ESP32 status information.
 */
struct hpi_hb_status_resp {
    uint8_t wifi_state;         /* 0=disconnected, 1=connected */
    uint8_t ble_state;          /* 0=idle, 1=advertising, 2=connected */
    uint8_t mqtt_state;         /* 0=disconnected, 1=connected */
    uint8_t buffer_usage_pct;   /* 0-100% */
    uint16_t frames_received;   /* Total frames received since boot */
    uint16_t frames_dropped;    /* Frames dropped due to errors */
} __attribute__((packed));

/* CRC lives in healthybridge_esp32_codec.h (hpi_hb_crc16). */

#ifdef __cplusplus
}
#endif

#endif /* HEALTHYBRIDGE_ESP32_SPI_PROTOCOL_H */
