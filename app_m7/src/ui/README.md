# ui/ — the on-device LVGL UI

The Material 3 LVGL UI: 5-slot bottom navigation (Home · Live · Rec · Trends ·
More), boot splash + self-test, ambient clock, and the More submenu (Link / HRV
/ Settings / Alert limits / OTA). Structure:

- `ui_module.c` — the engine: owns the LVGL thread, the screen manager + nav,
  the boot state machine, the ambient idle timeout, and the sample-bus drain.
- `screens/` — one file per screen; each exposes `create()` plus optional
  `refresh()`/`push_*()` hooks called from the UI thread.
- `components/` — shared M3 widgets (appbar, navbar, waveform, tiles).
- `theme/hpi_m3_theme.h` — every color/font/shape token. **Screens use tokens
  only; no per-screen hex.**
- `fonts/` — committed LVGL bitmap fonts + the regeneration pipeline
  (`fonts/README.md`).

The UI is optional: build-gated by `CONFIG_HPI_DISPLAY_ENABLED` (set via
`disp_conf.conf`); when off, `ui_module.c` self-stubs and the image is headless.
Every UI-facing control has a non-display path (HealthyPi Studio / the
`healthypi` CLI over CDC1, the `.HP6` stream on CDC0, MSC Transfer Mode), so a
headless build loses only the on-device preview.

## Contract this layer honors

L5 (UI). UI is a **bus subscriber** (live preview ring) and a **service client**
(calls `services/` for record/Transfer/status). It must never be on a producer's
callback path and must never be included by `core/` or `healthylink/`
(CI-enforced dependency rule). LVGL is called **only** from the UI thread —
bus data is drained in the UI loop, never rendered from a callback.

See [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md) for how this layer fits.
