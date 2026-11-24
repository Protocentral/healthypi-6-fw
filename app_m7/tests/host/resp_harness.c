/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host harness for the respiration-rate detector. #includes
 * app_m7/src/core/resp_rate.c itself, so the code under test is the code the
 * M7 runs, and drives its per-sample path (resp_on_sample) with 500 Hz ECG
 * samples read from stdin, one per line:
 *
 *   <resp_uv> <lead_off>
 *
 * Every 500 samples (1 s) it prints "R <t_s> <rr_bpm> <env>". test_resp_rate.py
 * drives it. Build with -DCONFIG_HPI_RESP_MIN_AMP_UV=<floor>.
 */
#define CONFIG_HPI_APP_LOG_LEVEL 0
#include "../../src/core/resp_rate.c"

#include <stdio.h>
#include <stdlib.h>

struct hpi_bus_sub *hpi_bus_subscribe(const struct hpi_bus_sub_cfg *cfg)
{
	(void)cfg;
	return NULL;
}

int hpi_bus_pull_wait(struct hpi_bus_sub *sub, struct hpi_sample_frame *out,
		      uint32_t timeout_ms)
{
	(void)sub; (void)out; (void)timeout_ms;
	return -1;
}

void hpi_bus_sub_get_stats(const struct hpi_bus_sub *sub,
			   struct hpi_bus_sub_stats *out)
{
	(void)sub; (void)out;
}

int main(void)
{
	char line[128];
	uint32_t n = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	resp_reset();
	while (fgets(line, sizeof(line), stdin)) {
		struct hp6_ecg_sample s = { 0 };
		char *end;

		s.resp = (int32_t)strtol(line, &end, 10);
		s.lead_off = (uint8_t)strtol(end, NULL, 10);
		resp_on_sample(&s, n * 2U);   /* 500 Hz -> 2 ms per sample */
		n++;
		if (n % 500U == 0U) {
			printf("R %u %u %d\n", n / 500U, hpi_resp_rate_bpm(), (int)env);
		}
	}
	return 0;
}
