"""Reference HRV values, written straight from the textbook definitions.

This is the test oracle for app_m4/src/algorithm_module.c. It deliberately
shares nothing with the firmware: plain Python floats, no CMSIS-DSP, no
integer shortcuts, a direct O(N^2) DFT. Standard library only, so the test
needs no packages and no downloads.

Definitions (Task Force of the ESC/NASPE, Circulation 1996;93:1043-1065):

  SDNN   standard deviation of the RR intervals. Population form, 1/N, which is
         what the firmware's comment states. (The 1/(N-1) form differs by
         < 0.2 % at N = 256 and < 7 % at the N = 8 minimum.)
  RMSSD  sqrt(mean((RR[i+1] - RR[i])^2)) over the N-1 successive differences.
  pNN50  100 * count(|RR[i+1] - RR[i]| > 50 ms) / (N-1).

Frequency domain, the conventional short-term method:

  1. Beat times t_k = cumulative sum of RR (s); RR_k is placed at t_k.
  2. Linear interpolation of RR(t) onto a uniform 4 Hz grid.
  3. Take the analysis segment: the most recent `n_max` samples (256 = 64 s),
     or all of them if there are fewer.
  4. Remove the mean, apply a symmetric Hann window of the segment's own
     length L: w[n] = 0.5 (1 - cos(2 pi n / (L-1))).
  5. Zero-pad to `nfft` (256) and take the DFT.
  6. One-sided PSD in ms^2/Hz:  P[k] = 2 |X[k]|^2 / (fs * sum(w^2)).
  7. Band power in ms^2: sum of P[k] * df over bins with lo <= f_k < hi,
     df = fs / nfft, DC excluded. Bands: VLF 0.003-0.04, LF 0.04-0.15,
     HF 0.15-0.4 Hz.

Sanity property used by the tests: for RR = m + A sin(2 pi f t) with f well
inside a band, that band's power is A^2 / 2 (the sinusoid's variance), to
within Hann leakage.
"""

import math

FS = 4.0
NFFT = 256
BANDS = {
    "vlf": (0.003, 0.04),
    "lf": (0.04, 0.15),
    "hf": (0.15, 0.4),
}


def mean(xs):
    return sum(xs) / len(xs)


def sdnn(rr):
    m = mean(rr)
    return math.sqrt(sum((x - m) ** 2 for x in rr) / len(rr))


def successive_diffs(rr):
    return [b - a for a, b in zip(rr, rr[1:])]


def rmssd(rr):
    d = successive_diffs(rr)
    return math.sqrt(sum(x * x for x in d) / len(d))


def pnn50(rr):
    d = successive_diffs(rr)
    return 100.0 * sum(1 for x in d if abs(x) > 50) / len(d)


def interpolate_4hz(rr):
    """RR (ms) onto a uniform FS grid from the first to the last beat time."""
    t = []
    acc = 0.0
    for x in rr:
        acc += x / 1000.0
        t.append(acc)
    out = []
    j = 0
    n = int(math.floor((t[-1] - t[0]) * FS)) + 1
    for i in range(n):
        ti = t[0] + i / FS
        while j < len(t) - 2 and t[j + 1] < ti:
            j += 1
        t0, t1 = t[j], t[j + 1]
        a = (ti - t0) / (t1 - t0)
        out.append(rr[j] + a * (rr[j + 1] - rr[j]))
    return out


def band_powers(rr, n_max=NFFT, segment="newest"):
    """Return dict of VLF/LF/HF band power (ms^2) and the segment length used."""
    x = interpolate_4hz(rr)
    if len(x) > n_max:
        x = x[-n_max:] if segment == "newest" else x[:n_max]
    L = len(x)
    m = mean(x)
    if L > 1:
        w = [0.5 * (1.0 - math.cos(2.0 * math.pi * n / (L - 1))) for n in range(L)]
    else:
        w = [1.0]
    xw = [(v - m) * wn for v, wn in zip(x, w)]
    s2 = sum(wn * wn for wn in w)
    df = FS / NFFT
    p = {k: 0.0 for k in BANDS}
    for k in range(1, NFFT // 2):
        f = k * df
        re = 0.0
        im = 0.0
        for n, v in enumerate(xw):
            ang = 2.0 * math.pi * k * n / NFFT
            re += v * math.cos(ang)
            im -= v * math.sin(ang)
        psd = 2.0 * (re * re + im * im) / (FS * s2)
        for name, (lo, hi) in BANDS.items():
            if lo <= f < hi:
                p[name] += psd * df
    p["samples"] = L
    return p


def lf_hf_derived(p):
    """LF/HF ratio and normalised units, as the firmware reports them."""
    lf, hf = p["lf"], p["hf"]
    ratio = lf / hf if hf > 0.001 else 0.0
    s = lf + hf
    lf_nu = 100.0 * lf / s if s > 0.001 else 0.0
    hf_nu = 100.0 * hf / s if s > 0.001 else 0.0
    return ratio, lf_nu, hf_nu


def sine_rr(n_beats, mean_ms, comps, quantum_ms=1):
    """RR series: interval k = mean + sum A sin(2 pi f t), t = time it starts.

    comps: list of (amplitude_ms, freq_hz). Values are rounded to quantum_ms
    (the firmware stores integer ms; RR from 500 Hz sample indices is 2 ms).
    """
    rr = []
    t = 0.0
    for _ in range(n_beats):
        v = mean_ms + sum(a * math.sin(2.0 * math.pi * f * t) for a, f in comps)
        v = int(round(v / quantum_ms)) * quantum_ms
        rr.append(v)
        t += v / 1000.0
    return rr
