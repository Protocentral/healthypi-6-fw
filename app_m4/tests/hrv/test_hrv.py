#!/usr/bin/env python3
"""Host test of the M4 HRV maths against an independent reference.

    python3 app_m4/tests/hrv/test_hrv.py

What it does
  1. Compiles app_m4/src/algorithm_module.c for the host (hrv_harness.c
     #includes it) with the real CMSIS-DSP sources from the west workspace,
     so the code under test is the code the M4 runs. Zephyr headers are
     replaced by the minimal shims in shim/.
  2. Feeds known RR-interval series in through update_rr_interval() - the one
     place a detected beat enters the HRV buffer - and calls the HRV functions.
  3. Compares every output with hrv_oracle.py, a from-the-definitions Python
     implementation that shares no code with the firmware.

Scope: this validates the HRV *maths* only. It does not exercise QRS
detection, its timing on hardware, or the 5 s / 60 s scheduling in the
algorithm thread. RR intervals go straight into the buffer.

Result states
  PASS   within tolerance.
  XFAIL  outside tolerance, and expected to be: a known defect, documented on
         the check with the measured numbers. Does not fail the run.
  FAIL   outside tolerance, not expected. Fails the run.
  XPASS  a known-defect check now passes. Fails the run, so the marker is
         removed in the same change that fixes the code.

CMSIS-DSP location: $CMSIS_DSP, else <west workspace>/modules/lib/cmsis-dsp
(the repo's parent directory is the workspace). Compiler: $CC, else cc.
"""

import math
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)

import hrv_oracle as ref  # noqa: E402

CMSIS_SOURCES = [
    "TransformFunctions/arm_rfft_fast_init_f32.c",
    "TransformFunctions/arm_rfft_fast_f32.c",
    "TransformFunctions/arm_cfft_f32.c",
    "TransformFunctions/arm_cfft_radix8_f32.c",
    "TransformFunctions/arm_cfft_init_f32.c",
    "TransformFunctions/arm_bitreversal2.c",
    "CommonTables/arm_common_tables.c",
    "CommonTables/arm_const_structs.c",
    "StatisticsFunctions/arm_mean_f32.c",
    "BasicMathFunctions/arm_offset_f32.c",
    "BasicMathFunctions/arm_mult_f32.c",
    "ComplexMathFunctions/arm_cmplx_mag_squared_f32.c",
    "FastMathFunctions/arm_cos_f32.c",
]

# --------------------------------------------------------------------------
# Known defects. Each is referenced by the checks it breaks.
# --------------------------------------------------------------------------

DEFECT_PARTIAL = (
    "D3: with 32-64 s of data (128-255 samples) the code multiplies the "
    "segment by the first L points of a 256-point Hann window - a truncated, "
    "one-sided taper - and normalises as if the full window had been applied."
)


# --------------------------------------------------------------------------
# Build + run
# --------------------------------------------------------------------------

def find_cmsis():
    env = os.environ.get("CMSIS_DSP")
    candidates = [env] if env else []
    candidates.append(os.path.join(REPO, "..", "modules", "lib", "cmsis-dsp"))
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "Include", "arm_math.h")):
            return os.path.abspath(c)
    sys.exit("CMSIS-DSP not found: set CMSIS_DSP=<path to cmsis-dsp checkout> "
             "(the west workspace has it at modules/lib/cmsis-dsp)")


def build(out_path):
    cmsis = find_cmsis()
    cc = os.environ.get("CC", "cc")
    cmd = [
        cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
        # The module logs size_t with %u; correct on the 32-bit M4, a format
        # warning on a 64-bit host. The log text is not under test.
        "-Wno-format",
        # Select CMSIS-DSP's generic (non-Cortex) compiler path on any host.
        "-D__GNUC_PYTHON__",
        "-I" + os.path.join(HERE, "shim"),
        "-I" + os.path.join(cmsis, "Include"),
        "-I" + os.path.join(cmsis, "PrivateInclude"),
        os.path.join(HERE, "hrv_harness.c"),
    ] + [os.path.join(cmsis, "Source", s) for s in CMSIS_SOURCES] + [
        "-lm", "-o", out_path,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit("host build of algorithm_module.c failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return cmsis


class Harness:
    def __init__(self, exe):
        self.exe = exe

    def run(self, cmds):
        r = subprocess.run([self.exe], input="\n".join(cmds) + "\n",
                           capture_output=True, text=True, check=True)
        return r.stdout.splitlines()


def kv(line):
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}


def feed(rr):
    return ["reset"] + ["rr %d" % x for x in rr]


def run_time(h, rr):
    out = h.run(feed(rr) + ["time", "vitals"])
    t = kv(next(l for l in out if l.startswith("TIME")))
    v = kv(next(l for l in out if l.startswith("VITALS")))
    return t, v


RAW_RE = re.compile(r"HRV freq raw: VLF=([-\d.]+), LF=([-\d.]+), HF=([-\d.]+)")


def run_freq(h, rr):
    out = h.run(feed(rr) + ["interp", "freq"])
    f = kv(next(l for l in out if l.startswith("FREQ")))
    f["samples"] = kv(next(l for l in out if l.startswith("INTERP")))["samples"]
    raw = None
    for l in out:
        m = RAW_RE.search(l)
        if m:
            raw = dict(zip(("vlf", "lf", "hf"), map(float, m.groups())))
    f["raw"] = raw
    return f


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

class Check:
    def __init__(self, case, metric, expected, actual, tol, xfail=None, note=""):
        self.case, self.metric = case, metric
        self.expected, self.actual, self.tol = expected, actual, tol
        self.xfail, self.note = xfail, note
        ok = abs(actual - expected) <= tol
        if self.xfail:
            self.status = "XPASS" if ok else "XFAIL"
        else:
            self.status = "PASS" if ok else "FAIL"


def fmt(x):
    if isinstance(x, float):
        return ("%.1f" % x) if abs(x) < 1e5 else ("%.3g" % x)
    return str(x)


def sinc(x):
    return 1.0 if x == 0 else math.sin(math.pi * x) / (math.pi * x)


def rel(expected, frac, floor):
    return max(abs(expected) * frac, floor)


# Tolerances, and why:
#  SDNN/RMSSD +-1 ms: the firmware truncates the mean to integer ms and takes
#    an integer (floor) square root, so each loses < 1 ms.
#  pNN50 +-1 %: integer division.
#  Band power +-10 %: interpolation alignment differs (the firmware places
#    RR_k at the start of its interval, the oracle at its end) and the two
#    segments differ by up to one sample at the edges.
#  LF/HF x10 +-max(2, 10 %), LFnu/HFnu +-3: same reasons, plus uint8 truncation.

def time_checks(case, rr, t, v=None, xfail=None):
    c = [
        Check(case, "valid", 1, t["valid"], 0, xfail),
        Check(case, "SDNN ms", ref.sdnn(rr), t["sdnn"], 1, xfail),
        Check(case, "RMSSD ms", ref.rmssd(rr), t["rmssd"], 1, xfail),
        Check(case, "pNN50 %", ref.pnn50(rr), t["pnn50"], 1, xfail),
        Check(case, "mean RR ms", ref.mean(rr), t["mean"], 1, xfail),
    ]
    if v is not None:
        c += [
            Check(case, "IPC hrv_sdnn", t["sdnn"], v["sdnn"], 0, xfail,
                  "payload field carries the computed value"),
            Check(case, "IPC hrv_rmssd", t["rmssd"], v["rmssd"], 0, xfail),
        ]
    return c


def freq_checks(case, rr, f, segment="newest", abs_xfail=None,
                rel_xfail=None, check_abs=True):
    p = ref.band_powers(rr, segment=segment)
    ratio, lf_nu, hf_nu = ref.lf_hf_derived(p)
    c = [Check(case, "freq valid", 1, f["valid"], 0)]
    if check_abs and f["raw"] is not None:
        # Absolute band powers. Before D1 was fixed (df applied twice) these
        # read exactly 1/64 of the oracle.
        for b in ("lf", "hf"):
            if p[b] > 1.0:
                c.append(Check(case, "%s power ms2" % b.upper(), p[b],
                               f["raw"][b], rel(p[b], 0.10, 1.0), abs_xfail))
    r10 = min(ratio * 10.0, 255.0)
    c += [
        Check(case, "LF/HF x10", r10, f["ratio_x10"], rel(r10, 0.10, 2.0),
              rel_xfail),
        Check(case, "LFnu %", lf_nu, f["lf_nu"], 3, rel_xfail),
        Check(case, "HFnu %", hf_nu, f["hf_nu"], 3, rel_xfail),
    ]
    return c, p


def cases(h):
    checks = []

    # ---- time domain ------------------------------------------------------

    rr = [1000] * 64
    t, v = run_time(h, rr)
    checks += time_checks("constant 1000 ms x64", rr, t, v)

    rr = ref.sine_rr(256, 1000, [(50, 0.10)])
    t, v = run_time(h, rr)
    checks += time_checks("sine A=50 ms @0.10 Hz (LF) x256", rr, t, v)
    checks.append(Check("sine A=50 ms @0.10 Hz (LF) x256", "SDNN vs analytic A/sqrt2",
                        50 / math.sqrt(2), t["sdnn"], 1.5,
                        note="analytic, independent of the oracle"))

    rr = ref.sine_rr(256, 1000, [(30, 0.25)])
    t, v = run_time(h, rr)
    checks += time_checks("sine A=30 ms @0.25 Hz (HF) x256", rr, t, v)
    checks.append(Check("sine A=30 ms @0.25 Hz (HF) x256", "SDNN vs analytic A/sqrt2",
                        30 / math.sqrt(2), t["sdnn"], 1.5,
                        note="analytic, independent of the oracle"))

    rng = random.Random(20261112)
    x, rr = 0.0, []
    for _ in range(180):
        x = 0.7 * x + rng.gauss(0.0, 30.0)
        rr.append(int(round(850 + x)))
    t, v = run_time(h, rr)
    checks += time_checks("random AR(1) seed 20261112 x180", rr, t, v)

    # Minimum beat count: 7 intervals -> invalid, 8 -> valid.
    rr7 = ref.sine_rr(7, 900, [(40, 0.2)])
    t, _ = run_time(h, rr7)
    checks.append(Check("7 intervals (< HRV_MIN_RR_COUNT)", "valid", 0, t["valid"], 0))
    rr8 = ref.sine_rr(8, 900, [(40, 0.2)])
    t, _ = run_time(h, rr8)
    checks += time_checks("8 intervals (= HRV_MIN_RR_COUNT)", rr8, t)

    # Circular buffer: 300 intervals, a 4 ms ramp. The buffer keeps the last
    # 256; if the wrap seam were mis-ordered RMSSD would see a ~1 s jump.
    rr = [400 + 4 * k for k in range(300)]
    t, _ = run_time(h, rr)
    checks += time_checks("ramp x300 (buffer wraps)", rr[-256:], t)

    # Eviction: large variability, then 256 constant intervals push it out.
    rr = [600, 1400] * 25 + [1000] * 256
    t, _ = run_time(h, rr)
    checks += time_checks("50 variable then 256 constant (evicted)", rr[-256:], t)

    # Ectopic beat in range: there is no ectopic/NN filtering, so it is
    # counted. Expected = the unfiltered definition (SDRR, really).
    rr = [1000] * 64
    rr[30], rr[31] = 600, 1400
    t, v = run_time(h, rr)
    nn = rr[:30] + rr[32:]
    checks += time_checks("one ectopic 600/1400 in 1000 x64", rr, t, v)
    checks[-1].note = ("no ectopic rejection: an NN-filtered RMSSD would be "
                       "%.1f ms, SDNN %.1f ms" % (ref.rmssd(nn), ref.sdnn(nn)))

    # Above 255 ms: the old uint8 HRV fields clipped these.
    rr = [600, 1400] * 32
    t, v = run_time(h, rr)
    checks += time_checks("alternating 600/1400 x64 (>255 ms)", rr, t, v)

    # ---- RR from QRS sample distance + physiological gate ------------------

    out = h.run(["gate %d" % d for d in (149, 150, 151, 500, 1000, 1001, 0)])
    got = {int(l.split()[1]): int(l.split()[2]) for l in out if l.startswith("GATE")}
    for d, exp in ((149, 0), (150, 300), (151, 302), (500, 1000),
                   (1000, 2000), (1001, 0), (0, 0)):
        checks.append(Check("gate: %d samples @500 Hz" % d, "RR ms (0=rejected)",
                            exp, got[d], 0))

    # ---- frequency domain -------------------------------------------------

    rr = [1000] * 64
    f = run_freq(h, rr)
    checks += [
        Check("freq: constant 1000 ms x64", "freq valid", 1, f["valid"], 0),
        Check("freq: constant 1000 ms x64", "LF+HF+VLF raw", 0.0,
              sum(f["raw"].values()), 1e-3),
        Check("freq: constant 1000 ms x64", "LF/HF x10", 0, f["ratio_x10"], 0),
    ]

    for name, comps in (
            ("freq: sine A=50 @0.10 Hz (LF), 64 s", [(50, 0.10)]),
            ("freq: sine A=30 @0.25 Hz (HF), 64 s", [(30, 0.25)]),
            ("freq: LF A=40 @0.10 + HF A=20 @0.25, 64 s", [(40, 0.10), (20, 0.25)])):
        rr = ref.sine_rr(64, 1000, comps)
        f = run_freq(h, rr)
        c, p = freq_checks(name, rr, f)
        checks += c
        # The oracle itself against the analytic answer. A sinusoid's power is
        # A^2/2, but it is sampled once per beat (T = 1 s here) and linearly
        # interpolated, whose response is sinc^2(f T) in amplitude, so the
        # power that survives is A^2/2 * sinc^4(f T): x0.94 at 0.10 Hz, x0.66
        # at 0.25 Hz. Any interpolating method shows this; it is not an error.
        expect = {}
        for a, fr in comps:
            band = "lf" if fr < 0.15 else "hf"
            expect[band] = a * a / 2.0 * sinc(fr * 1.0) ** 4
            checks.append(Check(name, "oracle %s vs analytic" % band.upper(),
                                expect[band], p[band], rel(expect[band], 0.05, 1.0),
                                note="checks the oracle, not the firmware: "
                                     "A^2/2 * sinc^4(f T)"))
        if len(comps) == 2:
            r10 = 10.0 * expect["lf"] / expect["hf"]
            checks.append(Check(name, "LF/HF x10 vs analytic", r10,
                                f["ratio_x10"], rel(r10, 0.10, 2.0),
                                note="(A_lf/A_hf)^2 * sinc^4 ratio"))

    rng = random.Random(7)
    x, rr = 0.0, []
    for _ in range(70):
        x = 0.6 * x + rng.gauss(0.0, 25.0)
        rr.append(int(round(900 + x)))
    f = run_freq(h, rr)
    checks += freq_checks("freq: random AR(1) seed 7 x70 (~63 s)", rr, f)[0]

    # Minimum data rules: >= 32 intervals AND >= 32 s (128 samples at 4 Hz).
    f = run_freq(h, ref.sine_rr(31, 1000, [(40, 0.1)]))
    checks.append(Check("freq: 31 intervals", "freq valid", 0, f["valid"], 0))
    f = run_freq(h, [600] * 40)
    checks.append(Check("freq: 40 intervals of 600 ms (24 s)", "freq valid",
                        0, f["valid"], 0, note="needs 32 s, not just 32 beats"))

    # 40 s of data: valid, but with the truncated-window path (D3).
    rr = ref.sine_rr(40, 1000, [(40, 0.10), (20, 0.25)])
    f = run_freq(h, rr)
    checks += freq_checks("freq: LF+HF mix, 40 s (partial window)", rr, f,
                          abs_xfail=DEFECT_PARTIAL)[0]

    # Window position: 128 beats of HF-only, then 128 beats of LF-only.
    # The most recent 64 s are pure LF. Before D2 was fixed the firmware
    # analysed the oldest 64 s instead and reported LF/HF 0.0, HFnu 99 %.
    rr = ref.sine_rr(128, 1000, [(30, 0.25)]) + ref.sine_rr(128, 1000, [(30, 0.10)])
    f = run_freq(h, rr)
    checks += freq_checks("freq: HF then LF, 256 beats (window position)", rr, f)[0]

    # The same after the circular buffer has wrapped: 200 beats of LF, then
    # 100 of HF. The buffer keeps the last 256 (156 LF + 100 HF); the newest
    # 64 s are pure HF.
    rr = ref.sine_rr(200, 1000, [(30, 0.10)]) + ref.sine_rr(100, 1000, [(30, 0.25)])
    f = run_freq(h, rr)
    checks += freq_checks("freq: LF then HF, 300 beats (wrapped, newest)",
                          rr[-256:], f)[0]

    return checks


def main():
    with tempfile.TemporaryDirectory() as d:
        exe = os.path.join(d, "hrv_harness")
        cmsis = build(exe)
        print("built app_m4/src/algorithm_module.c for host; CMSIS-DSP: %s\n" % cmsis)
        checks = cases(Harness(exe))

    w_case = max(len(c.case) for c in checks)
    w_metric = max(len(c.metric) for c in checks)
    hdr = "%-*s  %-*s  %10s  %10s  %8s  %s" % (w_case, "case", w_metric, "metric",
                                             "expected", "actual", "tol", "result")
    print(hdr)
    print("-" * len(hdr))
    defects = []
    for c in checks:
        print("%-*s  %-*s  %10s  %10s  %8s  %s" % (
            w_case, c.case, w_metric, c.metric, fmt(c.expected), fmt(c.actual),
            fmt(c.tol), c.status))
        if c.note:
            print("%*s  note: %s" % (w_case, "", c.note))
        if c.xfail and c.xfail not in defects:
            defects.append(c.xfail)

    counts = {s: sum(1 for c in checks if c.status == s)
              for s in ("PASS", "XFAIL", "FAIL", "XPASS")}
    print("\n%(PASS)d passed, %(XFAIL)d expected failures, %(FAIL)d failed, "
          "%(XPASS)d unexpectedly passed" % counts)
    if defects:
        print("\nKnown defects behind the expected failures:")
        for dsc in defects:
            print("  - " + dsc)
    return 1 if counts["FAIL"] or counts["XPASS"] else 0


if __name__ == "__main__":
    sys.exit(main())
