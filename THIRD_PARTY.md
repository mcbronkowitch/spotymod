# Third-Party Licenses

This project bundles or depends on the components listed below. Each retains its
own copyright and license; the notices in the respective source files are
authoritative. All linked components are permissively licensed (MIT). Between M1.6 and M4.5
the reverb linked DaisySP's separately-licensed LGPL `ReverbSc` module; as of
M4.5 the reverb is a vendored MIT Oliverb port and **no LGPL code is compiled
or linked** — the note below is retained for history.

The Spotykach firmware itself is distributed under the MIT License — see
[`LICENSE`](LICENSE) (Copyright © 2026 Synthux Academy). This repository is a
fork of [Synthux-Academy/Spotykach](https://github.com/Synthux-Academy/Spotykach).

## Vendored (source included in this repository)

Located under `third_party/`.

| Component | Version / Source | License | Copyright |
|-----------|------------------|---------|-----------|
| **doctest** | `third_party/doctest/doctest.h` | MIT | © 2016–2023 Viktor Kirilov |
| **nlohmann/json** | `third_party/nlohmann/json.hpp` | MIT | © 2013–2023 Niels Lohmann |
| **Oliverb** (Clouds Parasite reverb) | `third_party/oliverb/` — from [mqtthiqs/parasites](https://github.com/mqtthiqs/parasites) `clouds/dsp/fx/` + [pichenettes/stmlib](https://github.com/pichenettes/stmlib) utilities | MIT | © 2014 Emilie Gillet, © 2015 Matthias Puech |

- **doctest** — single-header C++ test framework. MIT License; see the header
  comment at the top of `third_party/doctest/doctest.h`
  (<https://opensource.org/licenses/MIT>). Portions are influenced by
  [Catch2](https://github.com/catchorg/Catch2) (Boost Software License 1.0).
- **nlohmann/json** — single-header JSON library. `SPDX-License-Identifier: MIT`.
  Embeds MIT-licensed sub-components: Grisu2/`dtoa` © 2009 Florian Loitsch, and a
  UTF-8 decoder © 2008–2009 Björn Höhrmann — both under permissive terms
  documented inline in `third_party/nlohmann/json.hpp`.
- **Oliverb** — the shared ambient reverb core (M4.5): `oliverb.h`,
  `fx_engine.h` (Emilie Gillet), `random_oscillator.h` (Matthias Puech), and
  `stmlib_shim.h` (trimmed stmlib utilities). Vendored **with modifications**,
  each listed in a comment block under the original MIT notice in the
  respective file (float32 buffer, 48 kHz constants, pitch shifter removed,
  per-sample processing, deterministic injected RNG).

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

As of M4.5 nothing in this repository compiles or links DaisySP-LGPL code:
the reverb moved to the vendored MIT Oliverb port under `third_party/oliverb/`,
and `ReverbSc`/`PitchShifter` were removed. The `DaisySP-LGPL/` directory
still exists inside the `lib/DaisySP` submodule checkout but is not part of
any build target.
