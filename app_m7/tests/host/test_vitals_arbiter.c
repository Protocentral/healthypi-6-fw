/*
 * Copyright (c) 2026 Protocentral Electronics
 * SPDX-License-Identifier: MIT
 *
 * Host tests for the M7 vitals arbitration and expiry rules
 * (app_m7/src/platform/vitals_arbiter.c).
 *
 * The rules are pure logic over timestamps, so they run without Zephyr. The
 * `sim_*` harness below plays the part of platform/ipc.c's vitals_work: run on
 * every message (always publish), and on the timer hpi_vit_next_expiry() asks
 * for (publish only if hpi_vit_arbitrated_differ()). That is how the
 * "publish once per transition, never periodically" property is checked.
 *
 * From the repo root:
 *
 *   cc -std=c11 -Wall -Wextra -Werror \
 *      -I app_m7/tests/host/stubs -I app_m7/src -I app_m7/src/platform \
 *      app_m7/tests/host/test_vitals_arbiter.c \
 *      app_m7/src/platform/vitals_arbiter.c \
 *      -o /tmp/vitals_arbiter_test && /tmp/vitals_arbiter_test
 */
#include <stdio.h>
#include <string.h>

#include "vitals_arbiter.h"

static int fails;

static void expect(const char *what, int cond)
{
	if (cond) {
		printf("ok   %s\n", what);
	} else {
		printf("FAIL %s\n", what);
		fails++;
	}
}

/* ---- a model of platform/ipc.c's vitals_work ----------------------------- */

static struct {
	struct hpi_vit_inputs in;
	bool leads_off;
	struct hp6_vitals pub;   /* last published sample */
	int publishes;
	int64_t timer;           /* HPI_VIT_NO_EXPIRY = not armed */
} sim;

static void sim_reset(void)
{
	memset(&sim, 0, sizeof(sim));
	sim.timer = HPI_VIT_NO_EXPIRY;
}

static void sim_run(int64_t now, bool arrived)
{
	struct hp6_vitals v = sim.pub;

	hpi_vit_arbitrate(&sim.in, now, sim.leads_off, &v);
	if (arrived || hpi_vit_arbitrated_differ(&v, &sim.pub)) {
		sim.pub = v;
		sim.publishes++;
	}
	sim.timer = hpi_vit_next_expiry(&sim.in, now);
	if (sim.timer != HPI_VIT_NO_EXPIRY && sim.timer <= now) {
		printf("FAIL next_expiry %lld not after now %lld\n",
		       (long long)sim.timer, (long long)now);
		fails++;
	}
}

/* Let time pass to `until`, firing the expiry timer whenever it is due. */
static void sim_advance(int64_t until)
{
	while (sim.timer != HPI_VIT_NO_EXPIRY && sim.timer <= until) {
		sim_run(sim.timer, false);
	}
}

static void sim_ecg(int64_t now, uint16_t hr, uint16_t sdnn)
{
	sim_advance(now);
	sim.in.ecg_hr = hr;
	sim.in.ecg_at = now;
	sim.in.hrv_sdnn_ms = sdnn;
	sim.in.hrv_rmssd_ms = sdnn / 2U;
	sim.in.hrv_lf_hf_x10 = 15;
	sim_run(now, true);
}

static void sim_ppg(int64_t now, uint16_t hr, uint8_t spo2, bool weak)
{
	sim_advance(now);
	sim.in.ppg_hr = hr;
	sim.in.spo2_x10 = (uint16_t)spo2 * 10U;
	sim.in.ppg_weak = weak;
	sim.in.ppg_at = now;
	sim_run(now, true);
}

/* ---- cases --------------------------------------------------------------- */

static void test_nothing_ever_arrives(void)
{
	struct hpi_vit_inputs in = {0};
	struct hp6_vitals v;

	memset(&v, 0xFF, sizeof(v));
	hpi_vit_arbitrate(&in, 60000, false, &v);
	expect("nothing: hr 0", v.hr_bpm == 0);
	expect("nothing: spo2 0", v.spo2_x10 == 0);
	expect("nothing: hrv 0", v.hrv_sdnn_ms == 0 && v.hrv_rmssd_ms == 0 &&
				 v.hrv_lf_hf_x10 == 0);
	expect("nothing: no provenance/weak flags",
	       (v.flags & (HP6_VIT_HR_FROM_PPG | HP6_VIT_PPG_WEAK |
			   HP6_VIT_ECG_LEAD_OFF)) == 0);
	expect("nothing: M7 fields untouched", v.temp_c_x100 == -1 &&
	       v.rr_bpm == 0xFFFF && (v.flags & HP6_VIT_MOTION));
	/* Early boot: k_uptime < STALE and every timestamp is 0. Zero values must
	 * still read as absent rather than "fresh". */
	expect("nothing at t=0: hr 0",
	       (hpi_vit_arbitrate(&in, 0, false, &v), v.hr_bpm == 0));
	expect("nothing: no timer", hpi_vit_next_expiry(&in, 0) == HPI_VIT_NO_EXPIRY &&
	       hpi_vit_next_expiry(&in, 60000) == HPI_VIT_NO_EXPIRY);

	sim_reset();
	sim_advance(3600000);
	expect("nothing: never publishes", sim.publishes == 0);
}

static void test_ecg_expires(void)
{
	sim_reset();
	sim_ecg(10000, 72, 300);
	sim_ecg(11000, 73, 310);
	expect("ecg: live HR", sim.pub.hr_bpm == 73 &&
	       !(sim.pub.flags & HP6_VIT_HR_FROM_PPG));
	expect("ecg: HRV full width", sim.pub.hrv_sdnn_ms == 310);
	expect("ecg: two messages, two publishes", sim.publishes == 2);

	sim_advance(11000 + HPI_VIT_STALE_MS);
	expect("ecg: still shown at exactly STALE", sim.pub.hr_bpm == 73 &&
	       sim.publishes == 2);

	sim_advance(11000 + HPI_VIT_STALE_MS + 1);
	expect("ecg: HR cleared 1 ms after STALE", sim.pub.hr_bpm == 0);
	expect("ecg: HRV cleared with it", sim.pub.hrv_sdnn_ms == 0 &&
	       sim.pub.hrv_rmssd_ms == 0 && sim.pub.hrv_lf_hf_x10 == 0);
	expect("ecg: exactly one expiry publish", sim.publishes == 3);
	expect("ecg: timer disarmed once cleared", sim.timer == HPI_VIT_NO_EXPIRY);

	sim_advance(3600000);
	expect("ecg: no further publishes while cleared", sim.publishes == 3);
}

static void test_ppg_fallback_then_expiry(void)
{
	sim_reset();
	sim_ecg(1000, 70, 50);
	sim_ppg(2000, 75, 97, false);
	expect("fallback: ECG wins while fresh", sim.pub.hr_bpm == 70 &&
	       !(sim.pub.flags & HP6_VIT_HR_FROM_PPG));
	expect("fallback: SpO2 alongside ECG", sim.pub.spo2_x10 == 970);

	/* M4 keeps sending PPG only; the ECG rate ages out. */
	sim_ppg(4000, 76, 98, false);
	int before = sim.publishes;

	sim_advance(1000 + HPI_VIT_STALE_MS + 1);
	expect("fallback: PPG takes over on ECG expiry", sim.pub.hr_bpm == 76 &&
	       (sim.pub.flags & HP6_VIT_HR_FROM_PPG));
	expect("fallback: HRV gone with the ECG", sim.pub.hrv_sdnn_ms == 0);
	expect("fallback: SpO2 kept", sim.pub.spo2_x10 == 980);
	expect("fallback: one publish for the switch", sim.publishes == before + 1);

	/* Then the M4 goes silent altogether. */
	sim_advance(4000 + HPI_VIT_STALE_MS + 1);
	expect("fallback: PPG HR cleared", sim.pub.hr_bpm == 0 &&
	       !(sim.pub.flags & HP6_VIT_HR_FROM_PPG));
	expect("fallback: SpO2 cleared", sim.pub.spo2_x10 == 0);
	expect("fallback: one publish for the clear", sim.publishes == before + 2);
	sim_advance(3600000);
	expect("fallback: quiet afterwards", sim.publishes == before + 2 &&
	       sim.timer == HPI_VIT_NO_EXPIRY);
}

static void test_spo2_and_weak_expire_without_hr(void)
{
	sim_reset();
	sim_ecg(1000, 70, 50);
	sim_ppg(1500, 0, 96, true);   /* SpO2 but no pulse rate, poor perfusion */
	expect("spo2: shown, weak flagged", sim.pub.spo2_x10 == 960 &&
	       (sim.pub.flags & HP6_VIT_PPG_WEAK));
	/* ECG keeps coming; the PPG message does not. */
	for (int64_t t = 2000; t <= 9000; t += 1000) {
		sim_ecg(t, 70, 50);
	}
	expect("spo2: cleared while ECG stays live", sim.pub.spo2_x10 == 0 &&
	       sim.pub.hr_bpm == 70);
	expect("spo2: PPG_WEAK goes with it", !(sim.pub.flags & HP6_VIT_PPG_WEAK));
}

static void test_lead_off(void)
{
	struct hpi_vit_inputs in = {
		.ecg_hr = 72, .ecg_at = 1000, .hrv_sdnn_ms = 40,
		.ppg_hr = 80, .spo2_x10 = 970, .ppg_at = 1000,
	};
	struct hp6_vitals v = {0};

	hpi_vit_arbitrate(&in, 1500, true, &v);
	expect("lead-off: ECG HR suppressed, PPG rate marked as PPG",
	       v.hr_bpm == 80 && (v.flags & HP6_VIT_HR_FROM_PPG));
	expect("lead-off: flag set", v.flags & HP6_VIT_ECG_LEAD_OFF);
	expect("lead-off: HRV suppressed", v.hrv_sdnn_ms == 0);

	in.ppg_hr = 0;
	hpi_vit_arbitrate(&in, 1500, true, &v);
	expect("lead-off, no PPG rate: HR not available",
	       v.hr_bpm == 0 && !(v.flags & HP6_VIT_HR_FROM_PPG));

	hpi_vit_arbitrate(&in, 1500, false, &v);
	expect("leads back on: ECG HR restored, flag cleared", v.hr_bpm == 72 &&
	       !(v.flags & (HP6_VIT_ECG_LEAD_OFF | HP6_VIT_HR_FROM_PPG)));

	/* The ECG input expiring while leads are off changes nothing visible:
	 * no publish for it. */
	sim_reset();
	sim.leads_off = true;
	sim_ecg(1000, 72, 40);
	expect("lead-off sim: ECG HR not shown", sim.pub.hr_bpm == 0 &&
	       (sim.pub.flags & HP6_VIT_ECG_LEAD_OFF));
	sim_advance(3600000);
	expect("lead-off sim: no redundant expiry publish", sim.publishes == 1);
}

static void test_late_message_restores(void)
{
	sim_reset();
	sim_ecg(1000, 72, 40);
	sim_ppg(1000, 74, 97, false);
	sim_advance(60000);
	expect("late: everything cleared", sim.pub.hr_bpm == 0 &&
	       sim.pub.spo2_x10 == 0);
	int before = sim.publishes;

	sim_ppg(60000, 78, 95, false);
	expect("late: PPG restores fallback HR + SpO2", sim.pub.hr_bpm == 78 &&
	       (sim.pub.flags & HP6_VIT_HR_FROM_PPG) && sim.pub.spo2_x10 == 950);
	sim_ecg(60500, 71, 45);
	expect("late: ECG restores ECG HR + HRV", sim.pub.hr_bpm == 71 &&
	       !(sim.pub.flags & HP6_VIT_HR_FROM_PPG) && sim.pub.hrv_sdnn_ms == 45);
	expect("late: published on each arrival", sim.publishes == before + 2);
	expect("late: expiry re-armed", sim.timer == 60000 + HPI_VIT_STALE_MS + 1);
}

static void test_next_expiry_picks_earliest(void)
{
	struct hpi_vit_inputs in = {
		.ecg_hr = 70, .ecg_at = 3000, .ppg_hr = 70, .ppg_at = 2000,
	};

	expect("next: earliest fresh input",
	       hpi_vit_next_expiry(&in, 4000) == 2000 + HPI_VIT_STALE_MS + 1);
	expect("next: skips an already-stale input",
	       hpi_vit_next_expiry(&in, 7500) == 3000 + HPI_VIT_STALE_MS + 1);
	expect("next: none once all stale",
	       hpi_vit_next_expiry(&in, 8001) == HPI_VIT_NO_EXPIRY);
}

int main(void)
{
	test_nothing_ever_arrives();
	test_ecg_expires();
	test_ppg_fallback_then_expiry();
	test_spo2_and_weak_expire_without_hr();
	test_lead_off();
	test_late_message_restores();
	test_next_expiry_picks_earliest();

	if (fails) {
		printf("\n%d FAILED\n", fails);
		return 1;
	}
	printf("\nall vitals arbiter tests passed\n");
	return 0;
}
