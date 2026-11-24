#!/usr/bin/env python3
"""Host test of the M7 respiration-rate detector (app_m7/src/core/resp_rate.c).

    python3 app_m7/tests/host/test_resp_rate.py
    python3 app_m7/tests/host/test_resp_rate.py --capture ecg.csv [--floor 20]

Builds resp_harness.c, which #includes resp_rate.c, and feeds it 500 Hz
respiration samples (uV, as acquisition.c publishes them) plus the lead-off
mask, one ECG sample at a time through the module's own resp_on_sample().

What it guards: the detector's high-pass started from x_prev = 0, so the
thoracic-impedance channel's DC level (~50 mV on v5) entered as a step. The
envelope caught it and the threshold (envelope / 4, 14 s decay) stayed
thousands of uV above any breath for ~5 minutes after boot and after every
limb-lead reconnect. Every case below starts on a 50 mV DC level, as the
hardware does.

Breaths here are 600 uV peak-to-peak, which the detector's filters leave at
roughly +-150-250 uV: clear of the 100 uV default floor
(CONFIG_HPI_RESP_MIN_AMP_UV), so this tests start-up and re-lock, not the
floor. At 300 uV the filtered peaks sit right at the floor and some are
missed (30 /min reads 15 at times). The floor still needs a real breathing
capture to set (see the Kconfig help); --capture replays one through the
detector at any floor.

Note the filters: high-pass ~0.08 Hz, two low-pass stages at ~0.45 Hz
(27 /min), while the accepted range runs to 40 /min -- fast breathing is
attenuated up to ~3x before it meets the floor.
"""

import argparse
import csv
import math
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.abspath(os.path.join(HERE, "..", ".."))
FS = 500
DC_UV = 50400
LIMB_OFF = 0x01  # HP6_LEAD_OFF_RA


def build(exe, floor):
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
           f"-DCONFIG_HPI_RESP_MIN_AMP_UV={floor}",
           "-I", os.path.join(HERE, "stubs"),
           "-I", os.path.join(APP, "src"),
           "-I", os.path.join(APP, "src", "core"),
           os.path.join(HERE, "resp_harness.c"), "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("host build of resp_rate.c failed:\n" + r.stderr)


def run(exe, samples):
    """samples: list of (resp_uv, lead_off). Returns [(t_s, rr, env)]."""
    inp = "".join(f"{v} {lo}\n" for v, lo in samples)
    r = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True)
    return [tuple(int(x) for x in l.split()[1:]) for l in r.stdout.splitlines()
            if l.startswith("R ")]


def synth(seconds, brpm, pp_uv=600, noise_uv=5, seed=1, hold_from=None,
          off=None):
    """Breathing at brpm on a DC level, with noise and cardiac crosstalk.
    hold_from: seconds after which breathing stops (breath hold).
    off: (start_s, end_s) during which RA reads off."""
    rnd = random.Random(seed)
    out = []
    for i in range(int(seconds * FS)):
        t = i / FS
        v = DC_UV + rnd.gauss(0, noise_uv) + 8 * math.sin(2 * math.pi * 1.2 * t)
        if brpm and (hold_from is None or t < hold_from):
            v += pp_uv / 2 * math.sin(2 * math.pi * brpm / 60 * t)
        lo = LIMB_OFF if off and off[0] <= t < off[1] else 0
        out.append((int(round(v)), lo))
    return out


def first_lock(res, want, after_s=0, tol=1):
    for t, rr, _ in res:
        if t > after_s and abs(rr - want) <= tol:
            return t
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", help="ECG CSV from `healthypi hp6 to-csv`")
    ap.add_argument("--floor", type=int, default=100)
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "resp_harness")
        build(exe, a.floor)

        if a.capture:
            rows = list(csv.DictReader(open(a.capture)))
            res = run(exe, [(int(float(r["resp_uv"])), int(r["lead_off"]))
                            for r in rows])
            print(f"floor {a.floor} uV: rr/env per second:")
            print(" ".join(f"{rr}/{env}" for _, rr, env in res))
            return 0

        fails = 0
        rows = []

        # From power-up, on a 50 mV DC level: locked by 25 s (3 intervals
        # plus the 0.08 Hz high-pass settling), and steady afterwards.
        for brpm in (10, 15, 20, 30):
            res = run(exe, synth(60, brpm))
            t = first_lock(res, brpm)
            steady = all(abs(rr - brpm) <= 1 for ts, rr, _ in res if ts >= 30)
            ok = t is not None and t <= 25 and steady
            rows.append((f"{brpm} /min from power-up", f"locked at {t} s, steady {steady}", ok))
            fails += not ok

        # A limb lead off for 3 s and back: locked again within 25 s of
        # reconnecting (resp_reset() re-seeds the filter).
        res = run(exe, synth(90, 15, off=(40, 43)))
        t = first_lock(res, 15, after_s=43)
        during = [rr for ts, rr, _ in res if 41 <= ts <= 43]
        ok = t is not None and t - 43 <= 25 and all(rr == 0 for rr in during)
        rows.append(("15 /min, RA off 40-43 s", f"0 while off; re-locked at {t} s", ok))
        fails += not ok

        # No breathing (noise and cardiac crosstalk only): never a rate.
        res = run(exe, synth(60, 0))
        ok = all(rr == 0 for _, rr, _ in res)
        rows.append(("no breathing", f"max rr {max(rr for _, rr, _ in res)}", ok))
        fails += not ok

        # Breath hold after 40 s: the rate withdraws (12 s stale + filter).
        res = run(exe, synth(80, 15, hold_from=40))
        late = [rr for ts, rr, _ in res if ts >= 60]
        ok = first_lock(res, 15) is not None and all(rr == 0 for rr in late)
        rows.append(("15 /min, breath hold from 40 s", f"rr from 60 s: {set(late)}", ok))
        fails += not ok

    for name, detail, ok in rows:
        print(f"{name:36s} {detail:40s} {'PASS' if ok else 'FAIL'}")
    print(f"\n{len(rows) - fails} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
