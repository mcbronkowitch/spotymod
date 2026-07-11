# Third-Party Licenses

This project bundles or depends on the components listed below. Each retains its
own copyright and license; the notices in the respective source files are
authoritative. All components are permissively licensed (MIT), with one optional
LGPL module set noted under DaisySP.

The Spotykach firmware itself is distributed under the MIT License — see
[`LICENSE`](LICENSE) (Copyright © 2026 Synthux Academy). This repository is a
fork of [Synthux-Academy/Spotykach](https://github.com/Synthux-Academy/Spotykach).

## Vendored (source included in this repository)

Located under `third_party/`.

| Component | Version / Source | License | Copyright |
|-----------|------------------|---------|-----------|
| **doctest** | `third_party/doctest/doctest.h` | MIT | © 2016–2023 Viktor Kirilov |
| **nlohmann/json** | `third_party/nlohmann/json.hpp` | MIT | © 2013–2023 Niels Lohmann |

- **doctest** — single-header C++ test framework. MIT License; see the header
  comment at the top of `third_party/doctest/doctest.h`
  (<https://opensource.org/licenses/MIT>). Portions are influenced by
  [Catch2](https://github.com/catchorg/Catch2) (Boost Software License 1.0).
- **nlohmann/json** — single-header JSON library. `SPDX-License-Identifier: MIT`.
  Embeds MIT-licensed sub-components: Grisu2/`dtoa` © 2009 Florian Loitsch, and a
  UTF-8 decoder © 2008–2009 Björn Höhrmann — both under permissive terms
  documented inline in `third_party/nlohmann/json.hpp`.

## Dependencies (referenced as git submodules — source NOT included here)

Located under `lib/`, pinned to specific upstream commits. Their code is fetched
separately (`git submodule update --init --recursive`) and remains under its own
license.

| Component | Source | License |
|-----------|--------|---------|
| **libDaisy** | [electro-smith/libDaisy](https://github.com/electro-smith/libDaisy) | MIT (© Electrosmith) |
| **DaisySP** | [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP) | MIT (© Electrosmith) |

### Note on DaisySP-LGPL

DaisySP ships an optional module set under `DaisySP/DaisySP-LGPL/` that is
licensed under the **LGPL**, separate from the MIT-licensed DaisySP core. If a
compiled firmware binary is distributed that links any DaisySP-LGPL module, the
LGPL's relinking/attribution obligations apply to that binary. Distributing this
**source** repository imposes no such obligation.

As of M1.6 the portable `engine/` core depends on DaisySP (MIT) for its FX
blocks — still no libDaisy — and `engine/fx/reverb.*` links the LGPL-2.1
`ReverbSc` module from `DaisySP-LGPL`. Desktop test/render binaries and any
distributed firmware binary therefore link LGPL code, with the relinking/
attribution obligations noted above. Distributing this source repository
imposes no such obligation.
