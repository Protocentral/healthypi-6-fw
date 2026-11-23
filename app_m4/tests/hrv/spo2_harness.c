/*
 * Host harness for the M4 SpO2 / PPG pulse-rate module.
 *
 * #includes app_m4/src/spo2_module.c itself, as hrv_harness.c does for the
 * ECG module, so the code under test is the code the M4 runs. Driven by
 * test_spo2.py over stdin, one command per line:
 *
 *   reset                     spo2_module_init()
 *   ppg r0 i0 r1 i1 ... (x16) one 16-sample PPG_RAW batch at 125 Hz (red, IR
 *                             raw, as the M7 sends them) through
 *                             spo2_accumulate_batch(). When it completes a
 *                             2 s window, prints the result as the algorithm
 *                             thread would send it (quality gate applied).
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../src/spo2_module.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t hpi_host_uptime_ms;
struct k_msgq q_ppg_batches;

int hpi_ipc_send(enum hpi_ipc_msg_type msg_type, const void *data, size_t len)
{
	(void)msg_type;
	(void)data;
	(void)len;
	return 0;
}

int hpi_ipc_register_callback(enum hpi_ipc_msg_type msg_type,
			      hpi_ipc_msg_callback_t callback, void *user_data)
{
	(void)msg_type;
	(void)callback;
	(void)user_data;
	return 0;
}

int main(void)
{
	static char line[1024];
	uint32_t samples = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);

	while (fgets(line, sizeof(line), stdin)) {
		char cmd[16] = {0};

		if (sscanf(line, "%15s", cmd) != 1) {
			continue;
		}
		if (strcmp(cmd, "reset") == 0) {
			spo2_module_init();
			samples = 0;
			printf("OK reset\n");
		} else if (strcmp(cmd, "ppg") == 0) {
			struct hpi_ipc_ppg_raw_batch b;
			struct spo2_algorithm_results r;
			const char *p = line + 3;
			char *end;

			memset(&b, 0, sizeof(b));
			b.sample_count = HPI_PPG_BATCH_SIZE;
			for (int i = 0; i < HPI_PPG_BATCH_SIZE; i++) {
				b.samples[i].red_raw = (int32_t)strtol(p, &end, 10);
				p = end;
				b.samples[i].ir_raw = (int32_t)strtol(p, &end, 10);
				if (end == p) {
					printf("ERR ppg needs %d red/ir pairs\n",
					       HPI_PPG_BATCH_SIZE);
					return 2;
				}
				p = end;
			}
			samples += HPI_PPG_BATCH_SIZE;
			hpi_host_uptime_ms = samples * 1000U / SPO2_SAMPLE_RATE_HZ;
			memset(&r, 0, sizeof(r));
			int ret = spo2_accumulate_batch(&b, &r);

			if (ret == 1) {
				/* The thread's quality gate (spo2_algorithm_thread_func) */
				bool good = r.signal_quality >= SPO2_MIN_QUALITY_SCORE;

				printf("W spo2=%u hr=%u q=%u pi=%u state=%u flags=0x%02x r_x1000=%d\n",
				       good ? r.spo2 : 0, good ? r.heart_rate : 0,
				       r.signal_quality, r.perfusion_index,
				       r.algorithm_state, r.flags,
				       (int)g_spo2_dbg_r_x1000);
			}
		} else {
			printf("ERR unknown command: %s", line);
			return 2;
		}
	}
	return 0;
}
