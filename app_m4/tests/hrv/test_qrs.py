#!/usr/bin/env python3
"""Host test of the M4 QRS detector: raw ECG in, beats and heart rate out.

    python3 app_m4/tests/hrv/test_qrs.py
    python3 app_m4/tests/hrv/test_qrs.py --capture cap_ecg.csv [--lead lead_i_uv]
    python3 app_m4/tests/hrv/test_qrs.py --coefficients

What it does
  Builds the same host harness as test_hrv.py (algorithm_module.c compiled for
  the host) and pushes 16-sample batches of raw Lead I microvolts through
  ecg_process_batch(): the band-pass, integrator, peak picking, thresholds,
  RR gate and heart-rate median that the M4 runs. Each case is a synthetic ECG
  whose beat times are known, so every detection is scored against the truth:
    sensitivity  true beats that were detected (after the 3 s start-up)
    PPV          detections that were true beats
    jitter       spread of (detection - true R) in samples; a constant lag is
                 expected (the integrator peak), its spread is HRV error
    HR           the rate the M4 reports at the end, vs the true rate

Why the cases are what they are
  "v5 250 Hz alternation" is the defect that kept firmware 1.0.0 from ever
  reporting a rate: the ADS1294R, mis-configured, added a constant offset to
  every other sample (96 / 209 / 148 uV on leads I / II / V1 on the unit
  captured 2026-11-23). The 1.0.0 detector fired on it every 256 ms and every
  interval was rejected. The others are the ordinary ways ECG goes wrong:
  mains, baseline wander, small or tall waves, amplitude steps, flat line,
  pauses, rate variability.

--capture replays a CSV from `healthypi hp6 to-csv` (columns t_ms, lead_i_uv,
...). There is no truth for it, so it reports beats found and the final rate
against a plain energy-detector estimate; it fails only if no rate comes out.
Captures are device data and are not kept in this repository.
"""

import argparse
import csv
import math
import os
import random
import statistics as st
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import test_hrv  # noqa: E402  (build() and find_cmsis())

FS = 500
B = 16


# --------------------------------------------------------------------------
# Synthetic ECG
# --------------------------------------------------------------------------

def gauss(t, mu, sigma, a):
    return a * math.exp(-0.5 * ((t - mu) / sigma) ** 2)


def synth(seconds, bpm, *, r_uv=1000.0, t_uv=300.0, noise_uv=15.0,
          wander_uv=0.0, mains_uv=0.0, mains_hz=50.0, parity_uv=0.0,
          rr_jitter_ms=0.0, amp_step=None, drop_beats=(), dc_uv=0.0,
          flat=False, seed=1):
    """Return (samples, true R sample indices)."""
    rnd = random.Random(seed)
    n = int(seconds * FS)
    x = [0.0] * n
    beats = []
    t = 0.4
    k = 0
    while t < seconds - 0.4:
        rr = 60.0 / bpm
        if rr_jitter_ms:
            # respiratory sinus arrhythmia-like modulation + a little noise
            rr += (rr_jitter_ms / 1000.0) * math.sin(2 * math.pi * 0.25 * t)
            rr += rnd.gauss(0, rr_jitter_ms / 4000.0)
        if k not in drop_beats and not flat:
            beats.append(t)
        k += 1
        t += rr
    for bt in beats:
        rr = 60.0 / bpm
        qt = 0.4 * math.sqrt(rr)
        amp = 1.0
        if amp_step and bt >= amp_step[0]:
            amp = amp_step[1]
        lo = max(0, int((bt - 0.35) * FS))
        hi = min(n, int((bt + qt + 0.2) * FS))
        for i in range(lo, hi):
            ti = i / FS
            v = (gauss(ti, bt - 0.16, 0.025, 0.15 * r_uv)      # P
                 + gauss(ti, bt - 0.022, 0.008, -0.10 * r_uv)  # Q
                 + gauss(ti, bt, 0.009, r_uv)                  # R
                 + gauss(ti, bt + 0.024, 0.009, -0.25 * r_uv)  # S
                 + gauss(ti, bt + qt * 0.75, 0.045, t_uv))     # T
            x[i] += amp * v
    for i in range(n):
        ti = i / FS
        x[i] += rnd.gauss(0, noise_uv) + dc_uv
        if wander_uv:
            x[i] += wander_uv * math.sin(2 * math.pi * 0.3 * ti)
        if mains_uv:
            x[i] += mains_uv * math.sin(2 * math.pi * mains_hz * ti + 0.3)
        if parity_uv and i % 2:
            x[i] -= parity_uv
    return [int(round(v)) for v in x], [int(round(b * FS)) for b in beats]


# --------------------------------------------------------------------------
# Harness
# --------------------------------------------------------------------------

def run_detector(exe, samples):
    cmds = ["reset"]
    for i in range(0, len(samples) - B + 1, B):
        cmds.append("ecg " + " ".join(str(v) for v in samples[i:i + B]))
    r = subprocess.run([exe], input="\n".join(cmds) + "\n",
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("harness failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
    dets, hr, last = [], 0, {}
    for line in r.stdout.splitlines():
        if not line.startswith("E "):
            continue
        f = dict(kv.split("=") for kv in line[2:].split())
        if f["q"] == "1":
            dets.append(int(f["idx"]))
        hr = int(f["hr"])
        last = f
    return dets, hr, last


def score(dets, truth, skip_s=3.0):
    """Match detections to true beats. A detection belongs to the true beat it
    follows by 0-250 ms (the integrator peak lags the R wave)."""
    skip = int(skip_s * FS)
    t_used = set()
    lags, fp = [], 0
    for d in dets:
        if d < skip:
            continue
        best = None
        for j, r in enumerate(truth):
            lag = d - r
            if -25 <= lag <= 125 and j not in t_used:
                best = j
                break
        if best is None:
            fp += 1
        else:
            t_used.add(best)
            lags.append(d - truth[best])
    # Beats too close to the end to have been confirmed do not count as missed.
    tp_truth = [j for j, r in enumerate(truth) if skip <= r < len_guard[0]]
    fn = sum(1 for j in tp_truth if j not in t_used)
    tp = len(lags)
    sens = tp / (tp + fn) if tp + fn else 1.0
    ppv = tp / (tp + fp) if tp + fp else (1.0 if not truth else 0.0)
    jit = st.pstdev(lags) if len(lags) > 1 else 0.0
    return tp, fp, fn, sens, ppv, jit


len_guard = [0]


# --------------------------------------------------------------------------
# Cases
# --------------------------------------------------------------------------

CASES = [
    # name, synth kwargs, expect: hr (None = must be 0), min sens, min ppv, max jitter
    ("clean 60 BPM", dict(bpm=60), 60),
    ("clean 75 BPM", dict(bpm=75), 75),
    ("clean 40 BPM", dict(bpm=40, seconds=40), 40),
    ("clean 120 BPM", dict(bpm=120), 120),
    ("clean 180 BPM", dict(bpm=180), 180),
    ("v5 250 Hz alternation, lead I (96 uV)", dict(bpm=80, parity_uv=96), 80),
    ("v5 250 Hz alternation, lead II (209 uV)", dict(bpm=80, parity_uv=209), 80),
    ("50 Hz mains 150 uV + wander 400 uV", dict(bpm=72, mains_uv=150, wander_uv=400), 72),
    ("60 Hz mains 150 uV", dict(bpm=72, mains_uv=150, mains_hz=60), 72),
    ("small QRS (250 uV)", dict(bpm=70, r_uv=250, t_uv=80, noise_uv=8), 70),
    ("tall T wave (T = 0.8 R)", dict(bpm=70, t_uv=800), 70),
    ("electrode DC offset 80 mV", dict(bpm=70, dc_uv=80000), 70),
    ("noisy (40 uV white)", dict(bpm=70, noise_uv=40), 70),
    ("RR variability +-80 ms", dict(bpm=65, rr_jitter_ms=80), 65),
    # A sudden drop to 0.3x leaves every new QRS (0.09x in the integrator)
    # under a threshold learnt on the old amplitude, and under searchback's
    # half-threshold too. Detection resumes when learning restarts 2.5 s after
    # the last QRS: ~5 beats lost at 75 BPM, as plain Pan-Tompkins would. The
    # rate is held meanwhile (it expires after 5 s without an interval).
    ("amplitude step down x0.3 at 12 s", dict(bpm=75, amp_step=(12.0, 0.3)), 75, 0.80),
    ("amplitude step up x4 at 12 s", dict(bpm=75, amp_step=(12.0, 4.0)), 75),
    ("one dropped beat (pause)", dict(bpm=70, drop_beats=(15,)), 70),
    ("flat line (leads off, 10 uV noise)", dict(bpm=70, flat=True, noise_uv=10), None),
    ("flat line + 250 Hz alternation", dict(bpm=70, flat=True, noise_uv=10, parity_uv=209), None),
]

MIN_SENS = 0.98
MIN_PPV = 0.98
MAX_JITTER = 3.0   # samples (6 ms); typical is ~1
HR_TOL = 2         # BPM


def run_cases(exe):
    fails = 0
    print(f"{'case':44s} {'beats':>6s} {'FP':>3s} {'FN':>3s} {'sens':>6s} "
          f"{'PPV':>6s} {'jit':>5s} {'HR':>4s} {'want':>4s}  result")
    for case in CASES:
        name, kw, want = case[:3]
        min_sens = case[3] if len(case) > 3 else MIN_SENS
        kw = dict(kw)
        seconds = kw.pop("seconds", 30)
        x, truth = synth(seconds, **kw)
        len_guard[0] = len(x) - int(0.3 * FS)
        dets, hr, _ = run_detector(exe, x)
        tp, fp, fn, sens, ppv, jit = score(dets, truth)
        if want is None:
            ok = hr == 0 and not [d for d in dets if d > 3 * FS]
            want_s = "0"
        else:
            ok = (sens >= min_sens and ppv >= MIN_PPV and jit <= MAX_JITTER
                  and abs(hr - want) <= HR_TOL)
            want_s = str(want)
        fails += not ok
        print(f"{name:44s} {tp:6d} {fp:3d} {fn:3d} {sens:6.3f} {ppv:6.3f} "
              f"{jit:5.2f} {hr:4d} {want_s:>4s}  {'PASS' if ok else 'FAIL'}")
    return fails


def energy_hr(x):
    d = [(x[i + 1] - x[i]) ** 2 for i in range(len(x) - 1)]
    w = int(FS * 0.08)
    e, s = [], sum(d[:w])
    for i in range(len(d) - w):
        e.append(s / w)
        s += d[i + w] - d[i]
    thr = st.mean(e) + 2.5 * st.pstdev(e)
    pk, last = [], -1e9
    for i in range(1, len(e) - 1):
        if e[i] > thr and e[i] >= e[i - 1] and e[i] >= e[i + 1] and i - last > 0.3 * FS:
            pk.append(i)
            last = i
    rr = [(b - a) / FS for a, b in zip(pk, pk[1:])]
    return 60 / st.median(rr) if rr else 0


def run_capture(exe, path, lead):
    rows = list(csv.DictReader(open(path)))
    x = [int(float(r[lead])) for r in rows]
    dets, hr, last = run_detector(exe, x)
    ref = energy_hr(x)
    rr = [(b - a) * 1000 // FS for a, b in zip(dets, dets[1:])]
    print(f"capture {os.path.basename(path)} [{lead}]: {len(x) / FS:.1f} s, "
          f"{len(dets)} QRS, final HR {hr} (energy-detector estimate {ref:.0f}), "
          f"thr {last.get('thr')}")
    print(f"  intervals ms: {rr[:20]}{' ...' if len(rr) > 20 else ''}")
    ok = hr > 0 and (ref == 0 or abs(hr - ref) <= 5)
    print("  PASS" if ok else "  FAIL")
    return 0 if ok else 1


def print_coefficients(fs=500.0):
    def rbj(kind, f0, q=1 / math.sqrt(2)):
        w = 2 * math.pi * f0 / fs
        a = math.sin(w) / (2 * q)
        c = math.cos(w)
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2] if kind == "lp" else \
            [(1 + c) / 2, -(1 + c), (1 + c) / 2]
        a0 = 1 + a
        return [v / a0 for v in b], [-2 * c / a0, (1 - a) / a0]
    for name, kind, f0 in (("qrs_hp", "hp", 5.0), ("qrs_lp", "lp", 15.0)):
        b, a = rbj(kind, f0)
        print(f"static const float {name}_b[3] = {{{', '.join('%.9ef' % v for v in b)}}};")
        print(f"static const float {name}_a[2] = {{{', '.join('%.9ef' % v for v in a)}}};")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", help="ECG CSV from `healthypi hp6 to-csv`")
    ap.add_argument("--lead", default="lead_i_uv")
    ap.add_argument("--coefficients", action="store_true",
                    help="print the band-pass coefficients for 500 Hz and exit")
    args = ap.parse_args()
    if args.coefficients:
        print_coefficients()
        return 0
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "hrv_harness")
        test_hrv.build(exe)
        if args.capture:
            return run_capture(exe, args.capture, args.lead)
        fails = run_cases(exe)
    print()
    print(f"{len(CASES) - fails} passed, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
