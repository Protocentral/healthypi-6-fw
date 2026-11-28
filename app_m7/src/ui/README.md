# ui/ — the on-device LVGL UI

The Material 3 LVGL UI: 5-slot bottom navigation (Home · Live · Rec · Trends ·
More), boot splash + self-test, ambient clock, and the More submenu (Link /
HealthyLink / HRV / Settings / Alert limits / OTA). Structure:

- `ui_module.c` — the engine: owns the LVGL thread, the screen manager + nav,
  the boot state machine, the ambient idle timeout, and the sample-bus drain.
- `screens/` — one file per screen; each exposes `create()` plus optional
  `refresh()`/`push_*()` hooks called from the UI thread.
- `components/` — shared M3 widgets (status bar, navbar, **button**,
  **segmented control**, stepper, status chip, waveform, tiles).
- `theme/hpi_m3_theme.h` — every color/font/shape token. **Screens use tokens
  only; no per-screen hex.**
- `fonts/` — committed LVGL bitmap fonts + the regeneration pipeline
  (`fonts/README.md`).

The UI is optional: build-gated by `CONFIG_HPI_DISPLAY_ENABLED` (set via
`disp_conf.conf`); when off, `ui_module.c` self-stubs and the image is headless.
Every UI-facing control has a non-display path (HealthyPi Studio / the
`healthypi` CLI over CDC1, the `.HP6` stream on CDC0, MSC Transfer Mode), so a
headless build loses only the on-device preview.

## Restyling

Everything visual is in **`theme/hpi_m3_theme.h`**, in five labelled sections:
surfaces, text, accents, buttons (`HPI_M3_BTN_*`) and interaction states
(`HPI_M3_PRESSED_LIGHTEN`, `HPI_M3_DISABLED_OPA`). Change a value there and
rebuild; no screen needs touching.

- **Buttons** are made only with `hpi_ui_button_create(parent, kind, icon,
  text, cb, user)`. Pick the kind by role — `CTA` (the one main action on a
  screen), `FILLED`, `TONAL`, `SECONDARY` (everything else), `DANGER` — never
  by colour. Do not call `lv_button_create()` in a screen, and do not colour a
  button's labels: the text colour is set on the button so the pressed and
  selected states reach them.
- **Choices** (tabs, one-of-N) are `SECONDARY` buttons with
  `hpi_ui_button_set_selected()`, or `hpi_ui_segmented_create()`.
- **Tappable rows/cards** that are not buttons call
  `hpi_m3_apply_pressable(obj, fill)` so a touch is visible.

The palette is tuned for the backlit TFT: surfaces start above the backlight
bleed floor (`#1E252C`, not near-black) and step far enough apart to read as
layers, and every button has a solid fill. When changing a colour, keep text
and accents ≥ 4.5:1 and control edges as strong as they are now, measured at
RGB565 (the UI renders 16-bit).

## Contract this layer honors

L5 (UI). UI is a **bus subscriber** (live preview ring) and a **service client**
(calls `services/` for record/Transfer/status). It must never be on a producer's
callback path and must never be included by `core/` or `healthylink/`
(CI-enforced dependency rule). LVGL is called **only** from the UI thread —
bus data is drained in the UI loop, never rendered from a callback.

See [`../../ARCHITECTURE.md`](../../ARCHITECTURE.md) for how this layer fits.
