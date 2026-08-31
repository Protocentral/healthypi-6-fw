License Information
===================

Hardware
---------

**All hardware is released under [Creative Commons Share-alike 4.0 International](http://creativecommons.org/licenses/by-sa/4.0/).**

You are free to:

Share — copy and redistribute the material in any medium or format
Adapt — remix, transform, and build upon the material
for any purpose, even commercially.
The licensor cannot revoke these freedoms as long as you follow the license terms.
Under the following terms:

Attribution — You must give appropriate credit, provide a link to the license, and indicate if changes were made. You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.
ShareAlike — If you remix, transform, or build upon the material, you must distribute your contributions under the same license as the original.
No additional restrictions — You may not apply legal terms or technological measures that legally restrict others from doing anything the license permits.
Notices:

You do not have to comply with the license for elements of the material in the public domain or where your use is permitted by an applicable exception or limitation.
No warranties are given. The license may not give you all of the permissions necessary for your intended use. For example, other rights such as publicity, privacy, or moral rights may limit how you use the material.


Software
--------

**Software written for this project is released under the MIT License
(http://opensource.org/licenses/MIT).**

> **Third-party components are NOT MIT, and this section does not relicense them.**
>
> **Individual files are authoritative: where a file carries an
> `SPDX-License-Identifier`, that identifier governs that file.** Every source
> file written for this project carries one. Since 2026-08-31 there is no tree
> here governed by a licence agreement shipped alongside it *instead* of a
> per-file tag — see the note below about the ST NPU runtime, which moved out
> that day. (Binary assets such as the bundled `.ttf` fonts cannot carry an SPDX
> tag; they are covered in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).)
>
> As published, the tree is:
>
> | Licence | What |
> |---|---|
> | **MIT** | software written for this project, including the host tooling under [`tools/`](tools/) |
> | **Apache-2.0** | files that are co-copyright with, or derived from, upstream **Zephyr, Linaro, STMicroelectronics, Nordic, NXP, Espressif** and others — board definitions, DT bindings and panel drivers. These **cannot** be relicensed: we are not the sole copyright holder. |
> | **OFL-1.1** | bundled fonts and their generated LVGL C arrays |
>
> **There is no proprietary code left in this repository.** Until 2026-08-31 it
> also carried 144 files of STMicroelectronics NPU runtime and generated model
> code under **ST SLA0104**, a proprietary Software Package License Agreement
> that is neither MIT nor open source — and whose clause 5 forbids
> redistribution under terms it defines to include MIT, which is exactly what
> this file declares. That tree went to
> [`Protocentral/healthylink-compute-fw`](https://github.com/Protocentral/healthylink-compute-fw)
> together with the rest of the STM32N657 compute-module firmware, and that
> repository's own `LICENSE.md` states the split terms. Nothing here is governed
> by SLA0104 any more.
>
> Fonts, their copyright holders and the verbatim licence texts are listed in
> [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md); the texts themselves are in
> [`app_m7/src/ui/fonts/LICENSES/`](app_m7/src/ui/fonts/LICENSES/). Zephyr, its
> modules and MCUboot are fetched by `west update` and licensed in their own
> repositories, not here.
>
> The per-file `SPDX-License-Identifier` is what actually governs. File counts
> were dropped from this table on 2026-08-31: they were last audited on
> 2026-08-03, they drift with every commit, and a stale number in a licence
> notice is worse than no number.

The MIT License (MIT)

Copyright (c) 2019 ProtoCentral

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
