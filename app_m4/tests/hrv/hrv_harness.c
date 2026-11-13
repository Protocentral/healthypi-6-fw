/*
 * Host harness for the M4 HRV maths.
 *
 * This file #includes app_m4/src/algorithm_module.c itself, so the functions
 * under test are the ones the M4 runs, compiled from the same source, with the
 * real CMSIS-DSP FFT. Only Zephyr's kernel/logging headers are replaced (see
 * shim/). Including the .c gives the harness the module's static state and
 * static functions without changing their linkage in the firmware.
 *
 * It reads one command per line on stdin and prints one result line per
 * command on stdout. test_hrv.py drives it; it is not meant to be run by hand,
 * but it can be:
 *
 *   printf 'reset\nrr 1000\n...\ntime\n' | ./hrv_harness
 *
 * Commands
 *   reset        algorithm_module_init(): clear all state (FFT init is once)
 *   rr <ms>      update_rr_interval(ms) - how a detected beat enters the buffer
 *   gate <n>     rr_ms_from_sample_delta(n): print the RR (ms) a QRS-to-QRS
 *                distance of n samples becomes, 0 if rejected
 *   time         calculate_hrv_time_domain() and print the result
 *   freq         calculate_hrv_freq_domain() and print the result (the module
 *                also logs its unclamped float band powers as "LOG INF HRV
 *                freq raw: ...", which the runner reads)
 *   hr           update_heart_rate(), as ecg_process_batch() calls it, and
 *                print the BPM it reports, its confidence and current_hr
 *   interp       print how many 4 Hz samples interpolate_rr_intervals() makes
 *   vitals       send_ecg_vitals() and print the HRV fields of the IPC payload
 *   hrv <0|1>    algorithm_set_hrv_enabled()
 *   batch [i]    one 16-sample ECG batch through the path that follows the QRS
 *                detector, as the algorithm thread runs it: advance the sample
 *                count by 16, set the uptime to sample time, track_rhythm()
 *                (RR, rate expiry, heart rate) with a QRS at index i of the
 *                batch if i is given, then ecg_report() (HRV passes + the
 *                decision to send ECG_VITALS). Prints the rate and, when an
 *                ECG_VITALS went out, the fields the M7 reads from it. The
 *                QRS detector itself is not run: beats are placed at exact
 *                sample indices.
 *
 * SPDX-License-Identifier: MIT
 */

#include "../../src/algorithm_module.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t hpi_host_uptime_ms;

/* --- IPC stand-ins: capture what the module would send to the M7 --------- */

static struct hpi_ipc_ecg_vitals captured_vitals;
static int captured_vitals_count;

int hpi_ipc_send(enum hpi_ipc_msg_type msg_type, const void *data, size_t len)
{
	if (msg_type == HPI_IPC_MSG_TYPE_ECG_VITALS && len == sizeof(captured_vitals)) {
		memcpy(&captured_vitals, data, len);
		captured_vitals_count++;
	}
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
	char line[128];

	setvbuf(stdout, NULL, _IOLBF, 0);

	while (fgets(line, sizeof(line), stdin)) {
		char cmd[16] = {0};
		unsigned long arg = 0;
		int n = sscanf(line, "%15s %lu", cmd, &arg);

		if (n < 1) {
			continue;
		}

		if (strcmp(cmd, "reset") == 0) {
			algorithm_module_init();
			printf("OK reset\n");
		} else if (strcmp(cmd, "rr") == 0 && n == 2) {
			update_rr_interval((uint16_t)arg);
		} else if (strcmp(cmd, "gate") == 0 && n == 2) {
			printf("GATE %lu %u\n", arg,
			       (unsigned)rr_ms_from_sample_delta((uint64_t)arg));
		} else if (strcmp(cmd, "time") == 0) {
			struct hrv_time_domain *h = &ecg_state.hrv;

			memset(h, 0, sizeof(*h));
			calculate_hrv_time_domain(h);
			printf("TIME valid=%u sdnn=%u rmssd=%u pnn50=%u mean=%u min=%u max=%u count=%u\n",
			       h->valid, h->sdnn_ms, h->rmssd_ms, h->pnn50,
			       h->mean_rr_ms, h->min_rr_ms, h->max_rr_ms,
			       (unsigned)ecg_state.rr_count);
		} else if (strcmp(cmd, "freq") == 0) {
			struct hrv_freq_domain *f = &ecg_state.hrv_freq;

			memset(f, 0, sizeof(*f));
			calculate_hrv_freq_domain(f);
			printf("FREQ valid=%u vlf=%u lf=%u hf=%u total=%u ratio_x10=%u lf_nu=%u hf_nu=%u\n",
			       f->valid, f->vlf_power_ms2, f->lf_power_ms2,
			       f->hf_power_ms2, f->total_power, f->lf_hf_ratio_x10,
			       f->lf_nu, f->hf_nu);
		} else if (strcmp(cmd, "hr") == 0) {
			/* The call ecg_process_batch() makes after every batch,
			 * start-up rule included (no new rate until rr_count >=
			 * HR_MIN_RR_COUNT); it sets ecg_state.current_hr, which
			 * ECG_VITALS.heart_rate carries to the M7. */
			struct ecg_algorithm_results r;

			memset(&r, 0, sizeof(r));
			update_heart_rate(&r);
			printf("HR %u count=%u conf=%u current=%u\n",
			       (unsigned)r.heart_rate, (unsigned)ecg_state.rr_count,
			       (unsigned)r.confidence, (unsigned)ecg_state.current_hr);
		} else if (strcmp(cmd, "interp") == 0) {
			size_t s = interpolate_rr_intervals(ecg_state.rr_intervals,
							    ecg_state.rr_count,
							    ecg_state.rr_index,
							    hrv_interp_buffer,
							    HRV_FFT_SIZE);
			printf("INTERP samples=%u\n", (unsigned)s);
		} else if (strcmp(cmd, "vitals") == 0) {
			captured_vitals_count = 0;
			send_ecg_vitals();
			printf("VITALS sent=%d sdnn=%u rmssd=%u lf=%u hf=%u ratio_x10=%u lf_nu=%u hf_nu=%u freq_valid=%u state=%u\n",
			       captured_vitals_count,
			       captured_vitals.hrv_sdnn, captured_vitals.hrv_rmssd,
			       captured_vitals.hrv_lf_power, captured_vitals.hrv_hf_power,
			       captured_vitals.hrv_lf_hf_ratio_x10,
			       captured_vitals.hrv_lf_nu, captured_vitals.hrv_hf_nu,
			       captured_vitals.hrv_freq_valid,
			       captured_vitals.algorithm_state);
		} else if (strcmp(cmd, "hrv") == 0 && n == 2) {
			algorithm_set_hrv_enabled(arg != 0);
		} else if (strcmp(cmd, "batch") == 0) {
			struct ecg_algorithm_results r;
			uint32_t now;

			ecg_state.sample_count += ECG_BATCH_SIZE;
			now = (uint32_t)(((uint64_t)ecg_state.sample_count * 1000U) /
					 HPI_ECG_M4_RATE_HZ);
			hpi_host_uptime_ms = now;
			memset(&r, 0, sizeof(r));
			r.timestamp = now;
			captured_vitals_count = 0;
			track_rhythm(n == 2,
				     (uint64_t)ecg_state.sample_count - ECG_BATCH_SIZE + arg,
				     &r);
			ecg_state.batches_processed++;
			ecg_report(&r, now);
			printf("B t=%u hr=%u conf=%u rr=%u q=%u hrvpass=%d tx=%d",
			       now, r.heart_rate, r.confidence, r.rr_interval_ms,
			       r.qrs_detected,
			       now != 0 && (ecg_state.last_hrv_update_ms == now ||
					    ecg_state.last_hrv_freq_update_ms == now),
			       captured_vitals_count);
			if (captured_vitals_count) {
				printf(" vhr=%u vconf=%u vstate=%u varr=%u vrr=%u",
				       captured_vitals.heart_rate,
				       captured_vitals.qrs_confidence,
				       captured_vitals.algorithm_state,
				       captured_vitals.arrhythmia_flags,
				       captured_vitals.rr_interval_ms);
			}
			printf("\n");
		} else {
			printf("ERR unknown command: %s", line);
			return 2;
		}
	}
	return 0;
}
