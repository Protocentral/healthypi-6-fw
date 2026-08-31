# Third-party notices

HealthyPi 6 firmware is MIT-licensed (see [LICENSE.md](LICENSE.md)), but the tree
bundles third-party assets under **other** licences. Those licences govern those
files, and this notice exists because MIT alone does not cover them.

Zephyr RTOS, its modules and MCUboot are **not** listed here — they are fetched by
`west update` and carry their own licences in their own repositories. This file
covers only what is committed to *this* repository.

Every copyright line below was read out of the font binary's `name` table
(nameID 0) rather than transcribed from a website, so it matches what the file
itself declares.

---

## Bundled fonts

Both the `.ttf` originals in [`app_m7/src/ui/fonts/ttf/`](app_m7/src/ui/fonts/ttf/)
and the generated LVGL C arrays in
[`app_m7/src/ui/fonts/lvgl/`](app_m7/src/ui/fonts/lvgl/) are covered. The C arrays
are **modified versions** in OFL terms (glyphs subset to a character range and
rasterised to 4 bpp bitmaps by `lv_font_conv`), which is permitted: OFL §2 allows a
modified version to be bundled with software provided the copyright notice and
licence travel with it. That is what this file and the per-file headers do.

None of these families declares a **Reserved Font Name**, so the OFL §3 renaming
requirement does not apply to the subset copies.

| Font | Licence | Copyright (from the binary) |
|---|---|---|
| Manrope (700) | SIL OFL 1.1 | Copyright 2019 The Manrope Project Authors (https://github.com/sharanda/manrope) |
| Rubik (500) | SIL OFL 1.1 | Copyright 2015 The Rubik Project Authors (https://github.com/googlefonts/rubik) |
| Saira (600, 700) | SIL OFL 1.1 | Copyright 2020 The Saira Project Authors (https://github.com/Omnibus-Type/Saira) |
| JetBrains Mono (Regular) | SIL OFL 1.1 | Copyright 2020 The JetBrains Mono Project Authors (https://github.com/JetBrains/JetBrainsMono) |
| Material Symbols Outlined | Apache-2.0 | Copyright 2026 Google LLC. All Rights Reserved. |

Licence texts, verbatim:
- [`app_m7/src/ui/fonts/LICENSES/OFL-1.1.txt`](app_m7/src/ui/fonts/LICENSES/OFL-1.1.txt)
- [`app_m7/src/ui/fonts/LICENSES/Apache-2.0.txt`](app_m7/src/ui/fonts/LICENSES/Apache-2.0.txt)

"Saira is a trademark of Omnibus-Type" — declared in the Saira binaries. A
trademark is not licensed by the OFL; the name is used here only to identify the
typeface.

**Material Symbols carries no licence field in its binary** (unlike the four OFL
fonts, which declare theirs in nameID 13). Apache-2.0 is asserted from its
upstream project, `google/material-design-icons`, which is Apache-2.0. If that
provenance ever needs to be defended, re-download from upstream and keep the
`LICENSE` file alongside it.

### A naming artifact, so nobody re-investigates it

The bundled statics were instanced from variable fonts and their `name` tables
were not rewritten, so the declared family disagrees with the filename:
`Manrope-700.ttf` says "Manrope ExtraLight", `Rubik-500.ttf` says "Rubik Light",
and **both** Saira files say "Saira Thin". The files are nonetheless correct and
distinct — `OS/2.usWeightClass` reads 700, 500, 600 and 700 respectively, and all
six have different SHA-256 digests. Verified 2026-07-25; treat the filename and
the weight class as authoritative, not the family string.

---

## Other third-party material

| Path | Origin | Note |
|---|---|---|
| `CMakeLists.txt`, `west.yml` | Nordic Semiconductor ASA | Zephyr module scaffolding; both carry Apache-2.0 SPDX headers, which govern. |

Vendored third-party clones, vendor datasheets and ML datasets are excluded from
this repository and are therefore not covered above — they are not published.

### The ST NPU runtime is no longer here

Until **2026-08-31** this table also listed two STMicroelectronics trees under
`app_healthylink_compute/src/` — 144 files of ATON NPU runtime and ST Edge AI
generated model code, governed by **ST SLA0104**, a proprietary Software Package
License Agreement that is neither open source nor MIT.

They were removed, not relicensed. SLA0104 clause 5 forbids redistributing the
package "in any manner that would subject the SOFTWARE PACKAGE to any Open
Source Terms", and the agreement's own definition of Open Source Terms names MIT
explicitly — which is what [LICENSE.md](LICENSE.md) declares for this
repository. The conflict was in the licence the *surrounding repository*
asserted, so the fix was to move the code, together with the rest of the
STM32N657 compute-module firmware, to
**[`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw)**.
That repository's root licence is not a blanket MIT grant, and it reproduces
SLA0104 verbatim alongside the ST tree.

Of the two, only one was ever built: `src/nn/`. The second,
`src/beat_classifier_stedge/`, was a complete duplicate of the ST runtime plus a
second generated model, referenced by no build file and `#include`d by nothing.
It was deleted during the split rather than carried across.

**Nothing in this repository is governed by SLA0104 any more.**

> **Removed from this table on 2026-08-03, having been checked:**
> `tools/extmemloader/` was listed with "prebuilt `.stldr` binaries" but does not
> exist in this repository and has no history here. `src/ring_buffer.c` was
> grouped with the Nordic scaffolding as "Zephyr-derived", pointing at SPDX
> headers it did not have; it is HealthyPi code (the M4↔M7 shared ring, built by
> `app_m4`) and now carries an MIT header of its own.
