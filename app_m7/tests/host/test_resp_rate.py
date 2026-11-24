#!/usr/bin/env python3
"""Host test of the M7 respiration-rate detector (app_m7/src/core/resp_rate.c).

    python3 app_m7/tests/host/test_resp_rate.py
    python3 app_m7/tests/host/test_resp_rate.py --capture ecg.csv [--floor 4]

Builds resp_harness.c, which #includes resp_rate.c, and feeds it 500 Hz
respiration samples (uV, as acquisition.c publishes them) plus the lead-off
mask, one ECG sample at a time through the module's own resp_on_sample().

The signal sizes are the bench's. A patient simulator at 20 breaths/min gave a
~20 uV p-p breath on a ~50.5 mV DC level with ~8 uV RMS of broadband noise
(data/sim_resp_20bpm.txt.gz, replayed below). The detector's low-pass leaves
that breath at only ~+-8 uV, and noise alone at ~1 uV, so the floor
(CONFIG_HPI_RESP_MIN_AMP_UV, default 4) sits between them.

What it guards, all of which kept respiration at 0 on 1.0.1:
  * Start-up: the high-pass started from x_prev = 0, so the DC level entered
    as a step and the threshold (envelope / 4, 14 s decay) stayed thousands of
    uV above any breath for ~5 minutes after boot and every limb-lead
    reconnect.
  * Fixed-point bias: q15_mul() floored, ~-0.5 uV per call, which settled into
    a 7-20 uV offset on the filtered signal -- as big as a breath. With noise
    present it made false "breaths"; on the simulator it hid the real ones
    once the floor was set low enough to see them.
  * Resolution: the filters ran in whole uV, a handful of steps across an
    8 uV wave. They now run in 1/16 uV.
  * The floor: 100 uV, 12x the filtered breath.

Known limits, printed but not asserted. The band is high-pass ~0.08 Hz and two
low-pass stages at ~0.45 Hz (27 /min), while the accepted range is 6-40 /min:
at bench amplitude, 6 /min and 40 /min are filtered below the floor and 30 /min
reads unsteadily. A larger real breath (a typical adult is 0.5-2 ohm) clears
both; retuning the band needs a capture of one.
"""

import argparse
import csv
import gzip
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
FLOOR = 4        # CONFIG_HPI_RESP_MIN_AMP_UV default (app_m7/Kconfig)
BREATH_PP = 20   # uV p-p, as measured on the simulator
NOISE = 8        # uV RMS at 500 Hz, as measured
SIM_CAPTURE = os.path.join(HERE, "data", "sim_resp_20bpm.txt.gz")


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
    """samples: list of (resp_uv, lead_off). Returns [(t_s, rr, env_uv)]."""
    inp = "".join(f"{v} {lo}\n" for v, lo in samples)
    r = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True)
    return [tuple(int(x) for x in l.split()[1:]) for l in r.stdout.splitlines()
            if l.startswith("R ")]


def synth(seconds, brpm, pp_uv=BREATH_PP, noise_uv=NOISE, seed=1,
          hold_from=None, off=None):
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


def sim_capture():
    with gzip.open(SIM_CAPTURE, "rt") as f:
        return [(int(l), 0) for l in f if not l.startswith("#")]


def first_lock(res, want, after_s=0, tol=1):
    for t, rr, _ in res:
        if t > after_s and abs(rr - want) <= tol:
            return t
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", help="ECG CSV from `healthypi hp6 to-csv`")
    ap.add_argument("--floor", type=int, default=FLOOR)
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

        def check(name, detail, ok):
            nonlocal fails
            rows.append((name, detail, "PASS" if ok else "FAIL"))
            fails += not ok

        # The bench capture: a simulator at 20 /min. Locked by 15 s and
        # reading 20 +-1 in at least 90% of the seconds after that.
        res = run(exe, sim_capture())
        t = first_lock(res, 20)
        after = [rr for ts, rr, _ in res if ts >= 15]
        good = sum(abs(rr - 20) <= 1 for rr in after)
        check("simulator capture, 20 /min",
              f"locked at {t} s, 20+-1 in {good}/{len(after)} s",
              t is not None and t <= 15 and good >= 0.9 * len(after))

        # From power-up, on a 50 mV DC level, at bench amplitude: locked by
        # 25 s (3 intervals plus the high-pass settling), steady afterwards.
        for brpm, tol in ((10, 1), (15, 1), (20, 2), (25, 1)):
            res = run(exe, synth(60, brpm))
            t = first_lock(res, brpm, tol=tol)
            late = sorted({rr for ts, rr, _ in res if ts >= 30})
            ok = t is not None and t <= 25 and \
                all(abs(rr - brpm) <= tol for rr in late)
            check(f"{brpm} /min from power-up", f"locked at {t} s, then {late}", ok)

        # A limb lead off for 3 s and back: 0 while off, locked again within
        # 25 s of reconnecting (resp_reset() re-seeds the filter).
        res = run(exe, synth(90, 15, off=(40, 43)))
        t = first_lock(res, 15, after_s=43)
        during = [rr for ts, rr, _ in res if 41 <= ts <= 43]
        check("15 /min, RA off 40-43 s", f"0 while off; re-locked at {t} s",
              t is not None and t - 43 <= 25 and all(rr == 0 for rr in during))

        # No breathing (noise and cardiac crosstalk only): never a rate, at
        # 2x the measured noise too.
        for noise in (NOISE, 2 * NOISE):
            res = run(exe, synth(120, 0, noise_uv=noise))
            check(f"no breathing, noise {noise} uV RMS",
                  f"max rr {max(rr for _, rr, _ in res)}, "
                  f"max env {max(e for _, _, e in res)} uV",
                  all(rr == 0 for _, rr, _ in res))

        # Breath hold after 40 s: the rate withdraws (12 s stale + filter).
        res = run(exe, synth(80, 15, hold_from=40))
        late = sorted({rr for ts, rr, _ in res if ts >= 60})
        check("15 /min, breath hold from 40 s", f"rr from 60 s: {late}",
              first_lock(res, 15) is not None and late == [0])

        # Known limits at bench amplitude (see the docstring): not asserted.
        for brpm in (6, 30, 40):
            res = run(exe, synth(60, brpm))
            late = sorted({rr for ts, rr, _ in res if ts >= 30})
            rows.append((f"{brpm} /min (known limit)", f"reads {late}", "info"))

    for name, detail, verdict in rows:
        print(f"{name:34s} {detail:44s} {verdict}")
    print(f"\nfloor {a.floor} uV: {sum(v == 'PASS' for *_, v in rows)} passed, "
          f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
