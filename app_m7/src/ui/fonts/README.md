# HealthyPi 6 UI fonts

The redesign UI uses the **real design type stack** (per
`docs/display/redesign/`), converted to LVGL bitmap fonts:

| Face | Role | Generated sizes |
|------|------|-----------------|
| **Rubik-500** | all numerals | `rubik_500_148` (focal HR/hero), `rubik_500_28` (chips) |
| **Manrope-700** | labels / units / caps | `manrope_700_16`, `manrope_700_12` |
| **Saira** (600/700) | screen titles | `saira_600_18` (title), `saira_700_28` (headline) |
| **JetBrains Mono** | filenames / versions | `jetbrains_mono_14` |
| **Material Symbols Outlined** | icons (nav / chips / status / caption) | `matsym_20`, `matsym_24` — 17-glyph subset |

- `ttf/` — the source TTFs (vendored from the design fonts: Rubik, Manrope,
  Saira, JetBrains Mono = **SIL OFL 1.1**; Material Symbols = **Apache-2.0**).
  Both licenses permit bundling/embedding.
- `LICENSES/` — the **verbatim licence texts**, which OFL-1.1 requires to
  accompany the font. Each generated `lvgl/*.c` also carries its font's copyright
  and SPDX identifier, because those arrays are *modified versions* in OFL terms
  (subset + rasterised) and OFL §2 only permits bundling one with software if the
  notice travels with it. Root summary: `THIRD_PARTY_NOTICES.md`. **These files
  are not covered by the project's licence — do not add a project MIT/Apache
  header to a generated font.**

> **Known naming artifact — verified, do not re-investigate.** The statics were
> instanced from variable fonts without rewriting their `name` tables, so the
> declared family disagrees with the filename: `Manrope-700.ttf` reports "Manrope
> ExtraLight", `Rubik-500.ttf` reports "Rubik Light", and **both** Saira files
> report "Saira Thin". The files are correct and distinct all the same —
> `OS/2.usWeightClass` reads 700 / 500 / 600 / 700 and all six digests differ
> Trust the filename and the weight class, not the family
> string.
- `hpi_symbols.h` — `HPI_SYM_*` UTF-8 string literals for the Material Symbols
  glyphs (codepoints extracted from the TTF cmap). Pair with a `matsym_*` font.
- `lvgl/*.c` — generated LVGL fonts (**checked in**, so a normal build needs no
  Node/tooling). Rubik uses a **numeric subset** (`space % + , - . / 0-9 :`);
  the text faces use printable ASCII (`0x20-0x7E`).
- `hpi_fonts.h` — `LV_FONT_DECLARE`s. The theme (`ui/theme/hpi_m3_theme.h`) maps
  its role tokens onto these; **screens use the tokens, never these names.**
- `convert_fonts.sh` — regenerates `lvgl/*.c` (needs `npx lv_font_conv`). Re-run
  after changing a size/range, then rebuild.

**Adding an icon:** add the glyph name to the `python3` cmap lookup, then its
codepoint to `SYM=` in `convert_fonts.sh` and a `HPI_SYM_*` macro in
`hpi_symbols.h`; re-run the script and rebuild.
