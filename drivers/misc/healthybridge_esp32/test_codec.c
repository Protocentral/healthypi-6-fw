/*
 * Copyright (c) 2026 Protocentral
 * SPDX-License-Identifier: MIT
 *
 * @file test_codec.c
 * @brief Host regression oracle for the HealthyBridge frame codec.
 *
 * Fails when healthybridge_esp32_codec.c drifts from the byte-identical copy
 * in the HealthyBridge ESP32 repository. A shifted field, length or CRC
 * coverage does NOT fail on the wire -- the ESP32 codec sync-hunts and would
 * mis-decode silently -- so the encoder is pinned against frozen byte vectors
 * and the decoder against round-trips. The ESP-side mirror is
 * test/test_hb_codec.c in the healthybridge-esp32 repository; keep both green.
 *
 * The codec is pure C, so it is testable without Zephyr. From the repo root:
 *
 *   cc -std=c11 -Wall -Wextra -Wno-unused-parameter \
 *      -I drivers/misc/healthybridge_esp32 \
 *      drivers/misc/healthybridge_esp32/test_codec.c \
 *      drivers/misc/healthybridge_esp32/healthybridge_esp32_codec.c \
 *      -o /tmp/hb_codec_test && /tmp/hb_codec_test
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "healthybridge_esp32_codec.h"

static int fails;

static void cmp(const char *what, const uint8_t *a, int la, const uint8_t *b, int lb)
{
	if (la != lb) { printf("FAIL %s: len %d vs %d\n", what, la, lb); fails++; return; }
	if (memcmp(a, b, (size_t)la) != 0) {
		printf("FAIL %s: bytes differ\n", what);
		for (int i = 0; i < la; i++)
			if (a[i] != b[i]) { printf("  first diff at %d: %02x vs %02x\n", i, a[i], b[i]); break; }
		fails++; return;
	}
	printf("ok   %s (%d B)\n", what, la);
}


/* ---- streaming decoder ------------------------------------------------- */

static uint8_t seen_type; static uint16_t seen_len, seen_seq; static int seen_n;
static uint8_t seen_pl[1024];
static void on_frame(uint8_t type, uint8_t flags, uint16_t seq,
		     const uint8_t *pl, uint16_t len, void *user)
{
	(void)flags; (void)user;
	seen_type = type; seen_len = len; seen_seq = seq; seen_n++;
	if (pl && len) memcpy(seen_pl, pl, len);
}

static void expect(const char *what, int cond)
{
	if (cond) printf("ok   %s\n", what);
	else { printf("FAIL %s\n", what); fails++; }
}

static void decoder_tests(void)
{
	static struct hpi_hb_parser P;
	static uint8_t F[4096];
	struct hpi_hb_vitals_payload v = { .hr_bpm = 60, .spo2_x10 = 970,
					    .rr_bpm = 12, .temp_c_x100 = 3650,
					    .flags = HPI_HB_VIT_HR_FROM_PPG };

	int n = hpi_hb_encode(F, sizeof(F), HPI_HB_MSG_TYPE_VITALS, 0, 0x2222,
			      (uint8_t *)&v, sizeof(v));

	/* 1. round trip */
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, F, (size_t)n, on_frame, NULL);
	expect("decode round-trip", seen_n == 1 && seen_type == HPI_HB_MSG_TYPE_VITALS &&
				    seen_len == sizeof(v) && seen_seq == 0x2222 &&
				    memcmp(seen_pl, &v, sizeof(v)) == 0);

	/* 2. split across chunk boundaries -- a case a UART hits constantly.
	 *    Feed one byte at a time. */
	hpi_hb_parser_reset(&P); seen_n = 0;
	for (int i = 0; i < n; i++) hpi_hb_parser_feed(&P, &F[i], 1, on_frame, NULL);
	expect("decode split byte-by-byte", seen_n == 1 && P.frames == 1);

	/* 3. leading garbage is hunted past, not fatal */
	static uint8_t G[4096];
	const uint8_t junk[] = { 0x00, 0xFF, 0x55, 0x12, 0xAA, 0x55 };
	memcpy(G, junk, sizeof(junk)); memcpy(G + sizeof(junk), F, (size_t)n);
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, G, sizeof(junk) + (size_t)n, on_frame, NULL);
	expect("sync-hunt past leading garbage", seen_n == 1);

	/* 4. a corrupt frame must not swallow the good frame behind it */
	memcpy(G, F, (size_t)n); G[10] ^= 0xFF;          /* break the payload */
	memcpy(G + n, F, (size_t)n);                      /* good frame follows */
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, G, (size_t)n * 2, on_frame, NULL);
	expect("corrupt frame -> crc_err, next frame still decoded",
	       seen_n == 1 && P.crc_errors == 1);

	/* 5. an absurd length field is rejected without consuming the stream */
	memcpy(G, F, (size_t)n); G[4] = 0xFF; G[5] = 0xFF;   /* length = 65535 */
	memcpy(G + n, F, (size_t)n);
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, G, (size_t)n * 2, on_frame, NULL);
	expect("oversize length -> resync, next frame decoded",
	       seen_n == 1 && P.resyncs >= 1);

	/* 6. back-to-back frames in one chunk */
	memcpy(G, F, (size_t)n); memcpy(G + n, F, (size_t)n); memcpy(G + 2*n, F, (size_t)n);
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, G, (size_t)n * 3, on_frame, NULL);
	expect("three frames in one chunk", seen_n == 3 && P.crc_errors == 0);

	/* 7. a full-size ECG frame decodes (largest thing on the link) */
	struct hpi_ecg_sample_multi e[16] = {0};
	for (int i = 0; i < 16; i++) e[i].ch1 = i * 12345;
	n = hpi_hb_encode_batch(F, sizeof(F), HPI_HB_MSG_TYPE_ECG_DATA, 1,
				42, 500, e, 16, sizeof(e[0]));
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, F, (size_t)n, on_frame, NULL);
	expect("max-size ECG frame round-trip",
	       seen_n == 1 && seen_len == 8 + sizeof(e) && n == 530);
}

/*
 * Frozen wire vectors, byte for byte, shared with the ESP side
 * (test/test_hp6_vitals.c and docs/PROTOCOL.md in healthybridge-esp32, 1.1.0).
 * If the header layout, a field order or the CRC coverage moves, these fail
 * here instead of silently mis-decoding on the far end.
 */

/* Vitals record A, seq 0: ECG HR 72, SpO2 98.5 %, RR 14, 36.85 degC,
 * SDNN 300 ms, RMSSD 38 ms, LF/HF 1.5, no flags. */
static const uint8_t VEC_VITALS_A[] = {
	0x55,0xAA,0x40,0x00,0x10,0x00,0x00,0x00,
	0x48,0x00,0xD9,0x03,0x0E,0x00,0x65,0x0E,0x2C,0x01,0x26,0x00,0x0F,0x00,0x00,0x00,
	0xE4,0x7D,
};

/* Vitals record B, seq 0: PPG pulse rate 71, SpO2 96.2 %, everything else
 * unavailable, all four flags set. */
static const uint8_t VEC_VITALS_B[] = {
	0x55,0xAA,0x40,0x00,0x10,0x00,0x00,0x00,
	0x47,0x00,0xC2,0x03,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0F,0x00,
	0x84,0x2C,
};

/* GET_VERSION request (CONTROL_CMD 0x50, cmd 0x31, no params), seq 0. */
static const uint8_t VEC_GET_VERSION_REQ[] = {
	0x55,0xAA,0x50,0x00,0x04,0x00,0x00,0x00,
	0x31,0x00,0x00,0x00,
	0x39,0x59,
};

/* GET_VERSION reply for "1.1.0" (CONTROL_RESP 0x51, status 0, 36 data bytes),
 * seq 0. */
static const uint8_t VEC_GET_VERSION_RESP[] = {
	0x55,0xAA,0x51,0x00,0x28,0x00,0x00,0x00,
	0x31,0x00,0x24,0x00,
	0x01,0x01,0x00,0x00,
	0x31,0x2E,0x31,0x2E,0x30,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0xA1,0x9B,
};

static void get_version_tests(void)
{
	static uint8_t B[64];
	static struct hpi_hb_parser P;

	/* Request, encoded the way the driver's hb_uart_command() builds it. */
	struct hpi_hb_control_cmd c = { .cmd_id = HPI_HB_CMD_GET_VERSION,
					.reserved = 0, .param_len = 0 };
	int n = hpi_hb_encode(B, sizeof(B), HPI_HB_MSG_TYPE_CONTROL_CMD, 0, 0,
			      (const uint8_t *)&c, sizeof(c));
	cmp("GET_VERSION request matches the frozen wire vector",
	    VEC_GET_VERSION_REQ, (int)sizeof(VEC_GET_VERSION_REQ), B, n);
	expect("GET_VERSION is 0x31", HPI_HB_CMD_GET_VERSION == 0x31);

	/* Reply: decode the golden frame and walk it the way hb_uart_on_frame()
	 * and hb_uart_api_get_version() do. */
	hpi_hb_parser_reset(&P); seen_n = 0;
	hpi_hb_parser_feed(&P, VEC_GET_VERSION_RESP, sizeof(VEC_GET_VERSION_RESP),
			   on_frame, NULL);
	expect("GET_VERSION reply decodes (CRC ok, type 0x51, 40 B)",
	       seen_n == 1 && P.crc_errors == 0 &&
	       seen_type == HPI_HB_MSG_TYPE_CONTROL_RESP && seen_seq == 0 &&
	       seen_len == sizeof(struct hpi_hb_control_resp) +
			   sizeof(struct hpi_hb_version_resp));

	struct hpi_hb_control_resp r;
	struct hpi_hb_version_resp v;

	memcpy(&r, seen_pl, sizeof(r));
	memcpy(&v, seen_pl + sizeof(r), sizeof(v));
	expect("GET_VERSION reply header: cmd 0x31, status OK, data_len 36",
	       r.cmd_id == HPI_HB_CMD_GET_VERSION && r.status == HPI_HB_RESP_OK &&
	       r.data_len == sizeof(struct hpi_hb_version_resp));
	expect("GET_VERSION reply body: 1.1.0, string \"1.1.0\", zero-padded",
	       v.major == 1 && v.minor == 1 && v.patch == 0 && v.reserved == 0 &&
	       strcmp(v.version_string, "1.1.0") == 0 &&
	       v.version_string[sizeof(v.version_string) - 1] == '\0');
}

int main(void)
{
	static uint8_t B[4096];

	/* 1. The dominant frame: 16 multi-channel ECG samples. */
	struct hpi_ecg_sample_multi ecg[16];
	for (int i = 0; i < 16; i++) {
		ecg[i].ch0 = -1000 + i; ecg[i].ch1 = 0x123456 + i; ecg[i].ch2 = -i * 7717;
		ecg[i].ch3 = i * 33; ecg[i].adc_ch1 = 0; ecg[i].adc_ch2 = 0;
		ecg[i].ppg_red = 0; ecg[i].ppg_ir = 0;
	}
	int n = hpi_hb_encode_batch(B, sizeof(B), HPI_HB_MSG_TYPE_ECG_DATA, 0x1234,
				    0xDEADBEEF, 500, ecg, 16, sizeof(ecg[0]));
	expect("ecg_multi x16 is 530 B on the wire", n == 530);
	expect("ecg header: sync + type",
	       B[0] == 0x55 && B[1] == 0xAA && B[2] == HPI_HB_MSG_TYPE_ECG_DATA);
	expect("ecg length field matches payload",
	       ((uint16_t)B[4] | ((uint16_t)B[5] << 8)) == 8 + sizeof(ecg));

	/* 2. PPG batch. */
	struct hpi_ppg_sample ppg[16];
	for (int i = 0; i < 16; i++) { ppg[i].red = 0x7FFFFF - i; ppg[i].ir = -0x800000 + i; }
	int m = hpi_hb_encode_batch(B, sizeof(B), HPI_HB_MSG_TYPE_PPG_DATA, 7,
				    12345678, 250, ppg, 16, sizeof(ppg[0]));
	expect("ppg x16 is 146 B on the wire", m == 146);

	/* 3. Vitals, pinned byte for byte against the frozen vectors. The
	 *    payload is the 16-byte hp6_vitals record, sent as-is. */
	expect("vitals payload is 16 B", sizeof(struct hpi_hb_vitals_payload) == 16);
	struct hpi_hb_vitals_payload v = { .hr_bpm = 72, .spo2_x10 = 985,
					   .rr_bpm = 14, .temp_c_x100 = 3685,
					   .hrv_sdnn_ms = 300, .hrv_rmssd_ms = 38,
					   .hrv_lf_hf_x10 = 15, .flags = 0 };
	int k = hpi_hb_encode(B, sizeof(B), HPI_HB_MSG_TYPE_VITALS, 0, 0,
			      (uint8_t *)&v, sizeof(v));
	cmp("vitals A matches the frozen wire vector",
	    VEC_VITALS_A, (int)sizeof(VEC_VITALS_A), B, k);

	struct hpi_hb_vitals_payload vb = {
		.hr_bpm = 71, .spo2_x10 = 962,
		.flags = HPI_HB_VIT_HR_FROM_PPG | HPI_HB_VIT_ECG_LEAD_OFF |
			 HPI_HB_VIT_PPG_WEAK | HPI_HB_VIT_MOTION };
	k = hpi_hb_encode(B, sizeof(B), HPI_HB_MSG_TYPE_VITALS, 0, 0,
			  (uint8_t *)&vb, sizeof(vb));
	cmp("vitals B (PPG rate, all flags) matches the frozen wire vector",
	    VEC_VITALS_B, (int)sizeof(VEC_VITALS_B), B, k);

	/* 4. CRC check value, independent of any frame. */
	expect("crc-16 check value 0x6F91",
	       hpi_hb_crc16((const uint8_t *)"123456789", 9) == 0x6F91);

	/* 5. Oversize payload refused, not truncated. */
	expect("oversize payload refused",
	       hpi_hb_encode(B, sizeof(B), 0x20, 0, 0, (uint8_t *)&v,
			     HPI_HB_MAX_PAYLOAD_SIZE + 1) < 0);

	/* 6. Undersized output refused. */
	expect("undersized output refused",
	       hpi_hb_encode_batch(B, 32, 0x20, 0, 0, 500, ecg, 16, sizeof(ecg[0])) < 0);

	decoder_tests();
	get_version_tests();

	printf(fails ? "\n%d FAILURE(S)\n" : "\nall passed\n", fails);
	return fails ? 1 : 0;
}
