# M4 Algorithm Core

**Scope:** `app_m4/` only (the Cortex-M4 calculations core). Assumes you already know
the HealthyPi 6 system (tri-core layout, OpenAMP IPC, build/flash scripts). This doc
covers **what the M4 actually does today, how it's wired, what's unfinished, and how
to test it.**

> ⚠️ **The M4's intended scope is wider than what is validated.** It is designed for QRS
> detection, heart rate, SpO₂, HRV and EEG band power. **The ECG→HR path is the only one
> that has been watched producing a plausible number.** HRV is on by default since
> 2026-08-31 and now runs on every device, but nothing has ever checked its output
> against a known RR series; SpO₂ is compiled in and receiving data but yields nothing
> (it decimates its input twice — see §4 F2); EEG is a stub. Details in §4.

---

## 1. Role & build

- **Cortex-M4 @ 200 MHz, C only**, calculations-only. No sensors, no display, no ML.
  (Beat/arrhythmia ML was removed in `b2c568f` and moved to the STM32N657 NPU module.)
- Receives raw sample batches from M7 over OpenAMP RPMSG, computes vitals, sends results
  back. Never touches hardware directly.
- **Build:** `scripts/build.sh m4` → `build/m4`.
- **Flash:** `scripts/flash.sh m4`.
- **Key Kconfig** (`app_m4/prj.conf`): OpenAMP RPMSG static vrings, CMSIS-DSP
  (transform/stats/fastmath for the HRV FFT), FPU + FPU_SHARING, `MAIN_STACK_SIZE=8192`,
  `HEAP_MEM_POOL_SIZE=16384`, `LOG_MODE_IMMEDIATE=y` (real-time logs), `STACK_SENTINEL=y`.
  No shell — M4 is observed purely through console logs.

---

## 2. Source map

| File | Lines | Role |
|------|------:|------|
| `src/main.c` | 547 | Boot, IPC bring-up, **PPG/EEG queues + work handlers + stats**, 10 s monitor loop |
| `src/ipc_module.c` / `.h` | 442 | OpenAMP REMOTE endpoint: init (7 s wait), send, callback dispatch table |
| `src/algorithm_module.c` / `.h` | 1460 | **The live path:** ECG QRS detection, HR, HRV time+freq domain |
| `src/spo2_module.c` / `.h` | 1590 | SpO2 + PPG-HR (HP5-derived R-ratio table). Compiled **in**; registers its own `PPG_RAW` callback. Produces nothing — see §4 F2 |
| ~~`src/spo2_algorithm.h`~~ | — | **Removed** (2026-08-30). Was the Maxim/HP5 SpO2 reference header; it was included by nothing. |
| ~~`src/test_ipc.c` / `.h`~~ | — | **Removed** (2026-06-09). Was the Phase-0 echo responder; IPC init now called directly from `main()` via `hpi_ipc_init()`. |
| `../src/ring_buffer.c` | — | Pulled in via `CMakeLists.txt` |

Shared contract with M7: `../app_m7/src/hpi_common_types.h` (batch & vitals structs,
`HPI_*_BATCH_SIZE`, algo-state/arrhythmia/flags enums). The IPC message-type enum is
duplicated in `app_m4/src/ipc_module.h` — **keep both in sync.**

---

## 3. Architecture & data flow

### IPC plumbing (the non-blocking pattern — don't break it)
```
M7 ──RPMSG──▶ hpi_ipc_ept_recv()  (ipc_module.c, IPC thread ctx)
                 └─ dispatch by msg->type to registered callback
                     └─ callback does ONLY: k_msgq_put(K_NO_WAIT) [+ k_work_submit]
                         └─ heavy work runs later in a thread/workqueue
```
Callbacks must return in <100 µs. The historical bug (Oct 2025): callbacks did ~5 ms of
work and overflowed IPC buffers after 1–4 min at ~187 msg/s. Fix = enqueue-only
callbacks draining into worker threads. **Any new algorithm must follow this.**

### Startup order
`main()` → `hpi_ipc_init()` (called directly; the Phase-0 `test_ipc.c` wrapper was
removed 2026-06-09) which **sleeps 7 s** (lets M7 finish display+HW init), opens `ipc0`,
registers the REMOTE endpoint, waits ≤15 s for M7 HOST to bind. **The 7 s delay is
load-bearing** — removing it deadlocks IPC. (`hpi_ipc_m4_init()` still exists but is
unused.)

### Message types (M7↔M4)
`0x10` PPG_RAW → / `0x11` PPG_VITALS ← / `0x12` PPG_CONFIG · `0x20` ECG_RAW → /
`0x21` ECG_VITALS ← / `0x22` ECG_CONFIG · `0x30` EEG_RAW → / `0x31` EEG_VITALS ← ·
`0x04` STATUS. `0x40–0x45` (arrhythmia/TFLite) are **vestigial/unused**. Convention:
raw batches M7→M4, `*_VITALS` M4→M7, `*_CONFIG` M7→M4. Batch size 16 samples for ECG/PPG,
8 for EEG. Keep messages ≤512 bytes (IPC MTU).

### Threads & queues
- **ECG path (live):** `algorithm_module` owns its **own** queue `q_ecg_algorithm` (32)
  and its own callback `ecg_batch_received_callback` (registered via
  `algorithm_register_ipc_callbacks()`). Processing runs in `ecg_algorithm_thread`
  (4 KB stack, prio 3), started suspended and released by `algorithm_module_start()`.
- **PPG/EEG path (main.c):** `main.c` defines `q_ppg/ecg/eeg_batches` + work handlers.
  Of these only the **EEG** callback is actually registered; its handler only does
  stats + sequence checking. The PPG and ECG handlers in `main.c` are **dead** (their
  callbacks are commented out — ECG is handled by `algorithm_module`, PPG by the
  disabled SpO2 module). This duplicate plumbing is confusing; consider deleting the
  dead halves.

---

## 4. Current state — what's real vs. aspirational

| Capability | State | Notes |
|------------|-------|-------|
| **ECG → QRS → HR** | ✅ **Live** | Only fully-working path. Simplified Pan-Tompkins on Lead I |
| **HRV time domain** (SDNN/RMSSD/pNN50) | 🟡 **Runs, not yet validated** | `HRV_ENABLED_DEFAULT=true` since 2026-08-31. Before that it defaulted false and nothing in either core ever called `algorithm_set_hrv_enabled(true)`, so SDNN/RMSSD were computed by no one and the fields they travel in read 0 on every device ever built. Updates every 5 s, needs ≥8 RR intervals. The numbers have never been compared against a known RR series |
| **HRV freq domain** (LF/HF via CMSIS-DSP FFT) | 🟡 **Runs, not yet validated** | Same default; updates every 60 s, needs ≥32 RR intervals. Never compared against a reference spectrum |
| **SpO2 + PPG-HR** | 🔴 **Runs, outputs nothing** | Compiled **in** (`main.c:483` is `#if 1`), starts, and registers `PPG_RAW` itself (`spo2_module.c:1285`). Broken by the double decimation in **F2** below |
| **EEG band power** | 🔴 **Stub** | `eeg_work_handler` only counts batches/sequence errors. `// TODO Phase 3: EEG frequency band analysis`. No FFT yet |
| **IPC ACKs to M7** | 🔴 Disabled | All `hpi_ipc_send(STATUS, ack)` blocks are `#if 0` — were a suspected buffer-saturation source |
| **Arrhythmia flags** | 🟡 Trivial | Only brady/tachy from HR thresholds in `send_ecg_vitals()`; real classification is on the NPU module |

### F2 — the SpO2 defect, in one place

The M7 **already** decimates PPG from 250 Hz to 125 Hz before sending it here
(`app_m7/src/platform/ipc.c`, `PPG_M4_RATE_HZ 125`, `PPG_DECIM 2`), and
`spo2_module.h` agrees: `SPO2_SAMPLE_RATE_HZ 125`.

But `spo2_module.c:82-85` still believes it is fed the raw AFE rate:

```c
#define HP6_SAMPLE_RATE     500     /* wrong twice: the AFE4400 runs at 250 Hz,
                                     * and the M4 is handed 125 Hz            */
#define HP5_SAMPLE_RATE     125
#define DOWNSAMPLE_RATIO    (HP6_SAMPLE_RATE / HP5_SAMPLE_RATE)  /* 4 */
```

and applies it for real at `spo2_module.c:1039-1044`, keeping one sample in four.
**125 / 4 = 31.25 Hz.** Everything downstream assumes 125 Hz: `SPO2_BUFFER_SIZE
250` is commented "2 seconds" but is 8; the DC filter is tuned for `fs=125 Hz`;
the peak-spacing gate of 10-250 samples means 30-200 BPM at 125 Hz but 7.5-50 BPM
at 31.25 Hz, so a normal pulse is rejected and no R ratio is ever computed.

The fix is to delete the second decimation, not to add a third rate constant.
Plan: `fw-internal-docs/release/RELEASE_1_0_0_FEATURE_COMPLETION_PLAN.md` W1.

### ECG algorithm specifics (for whoever tunes it)
- **Pipeline** (`ecg_process_batch`): extract Lead I (`samples[i].ecg_lead1`) → bandpass
  (2nd-difference high-pass using the 256-sample circular buffer for history) → 5-pt
  derivative → square → 8-sample moving-window integration → `qrs_detect()`.
- **Detection** is adaptive dual-threshold (signal/noise running estimates) with a
  learning phase (`QRS_LEARNING_BEATS=5`), 250 ms refractory, and a "QRS timeout" reset
  if none seen for >100 batches. ECG samples are in **µV** (R-peak ~500–2000 µV).
- **Known limitations to be aware of before improving it:**
  1. **RR intervals are derived from QRS sample indices** (fixed 2026-08-31) — and must
     stay that way. They used to be a difference of `k_uptime_get_32()` readings taken
     when a batch was *processed*: a batch is 16 samples = 32 ms at 500 Hz, so every RR
     carried batch quantisation plus the workqueue's scheduling jitter. SDNN and RMSSD
     in a healthy adult at rest are tens of milliseconds — the same order as that error
     — so the numbers looked plausible while largely measuring the scheduler. HR
     survived it (averaged over ≥3 intervals); HRV cannot. RR is now
     `sample_count - ECG_BATCH_SIZE + qrs_index` differenced and converted with
     `HPI_ECG_M4_RATE_HZ` (shared, in `app_m7/src/hpi_common_types.h`), giving 2 ms
     resolution set by the ADC. `last_qrs_time_ms` is retained but now only gates the
     refractory period — **do not reattach it to RR.**
  2. Derivative/square/integration operate on the 16-sample batch with edge clamping;
     only the bandpass stage uses cross-batch history. Continuity at batch boundaries
     is imperfect.
  3. Filter coefficients (`bp_filter_b/a`) are declared but the actual filter is a
     hand-rolled 2nd difference — the coeff arrays are unused.
- **Output:** `send_ecg_vitals()` packs `struct hpi_ipc_ecg_vitals` (HR, RR, QRS count,
  confidence, signal quality, HRV fields, `algorithm_state`, arrhythmia/flags) and sends
  `0x21`. Triggered on QRS, on HR change, or right after an HRV calc.

---

## 5. Suggested pick-up order for the unfinished work

1. **Fix + validate SpO2** — there is no `#if 0` to flip; the module is already
   compiled in and already receives PPG. Delete the second decimation (**F2**
   above), re-check every window constant against a real 125 Hz, then verify
   R-ratio→SpO2 against a reference oximeter. The calibration constants
   `SPO2_CALIBRATION_A/B` are inherited from HP5 and have never been measured on
   this AFE and optical stack — do not present SpO2 as a clinical number until
   they have been.
2. **Validate HRV** — there is nothing left to enable; it is on by default and the RR
   timebase is now the sample index, so what is missing is evidence the numbers are
   right. Feed a known RR series with published SDNN/RMSSD/LF-HF and compare against
   what the M4 emits, on both the 5 s time-domain and 60 s frequency-domain paths.
   Optionally add the `0x22 ECG_CONFIG` handler (or an M7 shell command) to call
   `algorithm_set_hrv_enabled()` for **runtime** control — the message type is reserved
   but unimplemented, and default-on is what shipped, so this is a convenience rather
   than a gate.
3. **Implement EEG band power** — replace the `eeg_work_handler` stub with a CMSIS-DSP
   FFT band-power calc (delta/theta/alpha/beta), emit `0x31 EEG_VITALS`.
4. **Decide on ACKs** — either delete the `#if 0` ACK blocks or re-introduce them with
   rate-limiting that doesn't saturate the IPC buffers.
5. **Clean the duplicate PPG/ECG plumbing** in `main.c` (dead handlers/queues).

---

## 6. Test process

M4 has **no shell**; everything is observed on the **console @ 115200** (logs interleave
with M7; filter on the `[M4]`/module tags — `algorithm_module`, `ipc_module_m4`,
`healthypi6_m4`). Use `LOG_MODE_IMMEDIATE` is on, so logs are real-time.

**Bring-up sanity (with M7 running):**
1. Flash both cores (`scripts/build.sh m4 && scripts/build.sh m7 && scripts/flash.sh`).
   M4 must boot *after/with* M7 — the 7 s delay handles ordering.
2. Look for `IPC endpoint bound - M4 REMOTE ready`. If you instead see
   `IPC endpoint binding timeout (15s)`, M7 didn't come up / wrong boot order.
3. With electrodes on, watch for `Learning beat N …`, then periodic
   `QRS: <n> detected, HR=<bpm> bpm` (every 500 batches ≈ 16 s) and
   `Sending ECG vitals: HR=…`.

**Health checks the firmware already prints:**
- The `main.c` 10 s monitor loop logs `Queue usage >50%` and `Message drops` — both
  should stay silent. Drops = the non-blocking pattern is being overrun.
- `algorithm_get_stats()` tracks `algorithm_overruns`, `queue_max_usage`,
  `processing_time_us_max/avg` — per-batch processing should be well under the
  ~32 ms inter-batch period (31.25 ECG batches/s).
- Sequence-error counters (`*_sequence_errors`) detect dropped/reordered batches from M7.

**IPC round-trip (Phase-0):** `test_ipc.c` registers an echo responder on `0x01 TEST` /
`0x02 DATA`. The M7-side `ipc_test loopback` / `ipc_test throughput` shell commands
(from the M7 console) exercise it end-to-end.

**HRV testing:** HRV is on by default, so `HRV calculated: SDNN=… RMSSD=…` / `HRV freq:
…` lines appear on their own once enough beats have accumulated — time domain needs ≥8 RR
intervals (every 5 s), frequency domain ≥32 (every 60 s). Seeing the lines only proves the
path runs; the values are unvalidated (§5.2).

**Pre-commit:** `scripts/build.sh m4` builds clean; IPC callbacks stay enqueue-only;
messages ≤512 B; plain `%u`/`%d` for `uint32_t`/`int32_t` in logs (both cores are
32-bit ARM, where `uint32_t` *is* `unsigned int`).

---

## 7. Gotchas specific to M4
- **Don't block in IPC callbacks** (§3) — the single most important rule here.
- **7 s boot delay is required** — IPC deadlocks without it.
- **HRV/SpO2/EEG appearing "broken"** is not one story, so check §4 before
  assuming which. **EEG** really is a stub. **HRV** is no longer disabled (on by
  default since 2026-08-31), so zeroed HRV fields are now a data or algorithm
  question, not a missing enable call. **SpO2** was never disabled either — it is
  compiled in and receiving data, and fails for a real reason (F2, the double
  decimation). Assuming a disable flag is what kept that defect hidden.
- **CMSIS-DSP FFT uses static RAM buffers** (`hrv_interp/window/fft_output/power_spectrum`,
  ~4 KB) and an init-once `arm_rfft_fast_instance_f32`; FFT runs in the ECG thread, so its
  4 KB stack must accommodate the call depth.
- **The message-type enum is duplicated** (M4 `ipc_module.h` vs M7 `hpi_common_types.h`).
  Edit both together.
