#!/usr/bin/env python3
"""Host test of the M4 SpO2 module: raw red/IR PPG in, SpO2 out.

    python3 app_m4/tests/hrv/test_spo2.py

Builds spo2_harness.c (which #includes app_m4/src/spo2_module.c) and pushes
16-sample PPG_RAW batches at 125 Hz -- what the M7 sends -- through
spo2_accumulate_batch(). Synthetic PPG: a systolic wave and a dicrotic wave
per beat, breathing wander and noise, with red and IR at independent DC levels
and a chosen true R = (AC_red/DC_red) / (AC_ir/DC_ir). The expected SpO2 is
Maxim's curve, -45.060 R^2 + 30.354 R + 94.845, which uch_spo2_table holds.

What it guards (firmware 1.0.0 read "--" on a clean PPG):
  * R must not depend on the DC levels. 1.0.0 added 2^21 to every sample
    and kept it in DC, so R ~ AC_red/AC_ir; it fell out of range unless red
    and IR sat at the same DC.
  * The table is indexed by R x 100. 1.0.0 used R x 100 - 50: R 1.0
    (SpO2 ~80%) read 99%, and a healthy R < 0.5 read "--".
  * No pulse means no number, and a lead-off flag.

Known limits, NOT asserted here (need a real finger capture to retune; see
the README in fw-internal-docs/algorithms/hr-debug-2026-11-23):
  * The PPG pulse rate halves above 80 BPM: peaks must be 0.75 s apart.
    SpO2 is unaffected (the AC/DC segments just span two beats).
  * Below ~50 BPM a 2 s window can hold one peak, and the reading drops out
    for that window.
"""

import math
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FS = 125
B = 16


def maxim(r):
    return -45.060 * r * r + 30.354 * r + 94.845


def pulse(ph):
    return (math.exp(-0.5 * ((ph - 0.18) / 0.07) ** 2)
            + 0.35 * math.exp(-0.5 * ((ph - 0.45) / 0.08) ** 2))


def synth(seconds, bpm, *, r=0.5, ac_ir=8000, dc_ir=150000, dc_red=60000,
          noise=0.02, flat=False, seed=1):
    rnd = random.Random(seed)
    ac_red = r * ac_ir * dc_red / dc_ir
    out = []
    for i in range(int(seconds * FS)):
        t = i / FS
        p = 0.0 if flat else pulse((t * bpm / 60) % 1)
        w = 0.0 if flat else math.sin(2 * math.pi * 0.25 * t)
        ir = dc_ir - ac_ir * p + 0.3 * ac_ir * w + rnd.gauss(0, noise * ac_ir)
        rd = dc_red - ac_red * p + 0.3 * ac_red * w + rnd.gauss(0, noise * ac_red)
        out.append((int(rd), int(ir)))
    return out


def build(exe):
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=gnu11", "-O1", "-Wall", "-Wno-unused-function",
           "-Wno-unused-variable", "-Wno-unused-but-set-variable",
           "-Wno-format", "-I" + os.path.join(HERE, "shim"),
           os.path.join(HERE, "spo2_harness.c"), "-lm", "-o", exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("host build of spo2_module.c failed:\n" + r.stderr)


def run(exe, x):
    cmds = ["reset"] + ["ppg " + " ".join(f"{a} {b}" for a, b in x[i:i + B])
                        for i in range(0, len(x) - B + 1, B)]
    r = subprocess.run([exe], input="\n".join(cmds) + "\n",
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("harness failed:\n" + r.stdout[-2000:])
    return [{k: int(v, 0) for k, v in (kv.split("=") for kv in l[2:].split())}
            for l in r.stdout.splitlines() if l.startswith("W ")]


def main():
    fails = 0
    rows = []
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "spo2_harness")
        build(exe)

        # R and SpO2 across DC levels and true R, at 72 BPM. Windows after the
        # first two (start-up) must each show the expected SpO2 +-2.
        for dc_red, dc_ir in ((60000, 150000), (150000, 150000), (300000, 150000),
                              (20000, 400000)):
            for r in (0.4, 0.5, 0.7, 1.0):
                w = run(exe, synth(20, 72, r=r, dc_red=dc_red, dc_ir=dc_ir))[2:]
                want = round(maxim(r))
                shown = [x["spo2"] for x in w]
                rs = [x["r_x1000"] / 1000 for x in w]
                ok = all(abs(s - want) <= 2 for s in shown) and \
                    all(abs(v - r) <= 0.1 * r for v in rs)
                rows.append((f"DC red {dc_red:6d} / IR {dc_ir:6d}, R {r}",
                             f"{want}", f"{min(shown)}-{max(shown)}",
                             f"{min(rs):.2f}-{max(rs):.2f}", ok))
                fails += not ok

        # Across heart rates (SpO2 only; the pulse rate is a known limit).
        for bpm in (60, 75, 100, 140):
            w = run(exe, synth(20, bpm, r=0.5))[2:]
            shown = [x["spo2"] for x in w]
            ok = all(abs(s - 99) <= 2 for s in shown)
            rows.append((f"{bpm} BPM, R 0.5", "99", f"{min(shown)}-{max(shown)}",
                         "", ok))
            fails += not ok

        # No pulse: no number, lead-off flag (HPI_PPG_FLAG_LEAD_OFF = 0x01).
        w = run(exe, synth(20, 72, flat=True, noise=0.002))[2:]
        ok = all(x["spo2"] == 0 and x["hr"] == 0 and x["flags"] & 0x01 for x in w)
        rows.append(("no pulse (DC only)", "0", f"{max(x['spo2'] for x in w)}",
                     "", ok))
        fails += not ok

    print(f"{'case':38s} {'want':>5s} {'shown':>7s} {'module R':>10s}  result")
    for name, want, shown, rs, ok in rows:
        print(f"{name:38s} {want:>5s} {shown:>7s} {rs:>10s}  {'PASS' if ok else 'FAIL'}")
    print(f"\n{len(rows) - fails} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
