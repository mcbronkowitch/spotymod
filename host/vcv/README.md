# spotymod — VCV Rack host

A third host over the shared portable engine (`engine/`), alongside the desktop
render host (`host/render/`) and the future Daisy firmware shell (M6). **No
engine code is duplicated**: this plugin compiles the exact same `engine/*.cpp`
sources against the VCV Rack SDK.

```
host/vcv/
├── Makefile              Rack plugin build (points SOURCES at ../../engine)
├── plugin.json           Rack plugin manifest
├── src/
│   ├── plugin.{hpp,cpp}  plugin entry + model registration
│   ├── Spotymod.cpp      Module: param→engine mapping, process(), widget
│   └── generated_panel.hpp   GENERATED (enums + control table)
└── res/
    ├── gen_panel.py      single source of truth for the panel layout
    └── Spotymod.svg      GENERATED faceplate
```

## Design: one control per function (VCV-native)

The hardware Spotykach drives its ~60 parameters through capacitive pads +
hold/combo gestures (a layer that is firmware milestone **M6**, not yet built).
VCV has no clean equivalent for pad gestures, so this host deliberately maps
**every engine setter to its own dedicated knob/switch/button** — a wider panel
than the hardware, but the full engine is playable today without waiting on M6.

The visual language follows the residency devlog ("workbench paper"): a warm
paper plate with ink lettering, solder-green accents on part A (left) and
copper-orange on part B (right); MORPH wears a split green/copper collar
because it bridges the two parts. Each part's
32-LED ring is a **live** custom widget (`SpkyRing` in `src/Spotymod.cpp`): it
draws in the light layer and lights a moving dot per modulation lane from
`Instrument::lane_output()` / `lane_fired()`, so the rings animate with the
engine (mirroring `src/ui/led.ring.h`). The SVG only provides the dim housing.

## Build

Requires the [VCV Rack SDK](https://vcvrack.com/manual/Building#Setting-up-the-Rack-SDK)
(v2), plus `make` and `jq`. The Makefile's default `RACK_DIR` is `../../../Rack-SDK`
(i.e. unzip the SDK next to the repo), or pass `RACK_DIR=/path/to/Rack-SDK`.

The shared engine is **C++17** (`std::clamp`, ...), but the SDK defaults to
`-std=c++11`; the Makefile bumps it back up via `EXTRA_CXXFLAGS += -std=c++17`,
so nothing extra is needed on your end.

```bash
# from this directory:
make            # -> plugin.dll / .so / .dylib
make install    # packages a .vcvplugin and copies it into Rack's user plugin dir
```

`make install` drops `Spotymod-<version>-<arch>.vcvplugin` into Rack's user dir
(`%LOCALAPPDATA%\Rack2\plugins-win-x64\` on Windows, `~/.local/share/Rack2/…` /
`~/Library/Application Support/Rack2/…` elsewhere); Rack unpacks it on launch.
Restart Rack and the module appears under the **Synthux Academy** brand
("Spotymod" in the module browser). A self-built plugin is unsigned, so Rack may
note it isn't from the library — it still loads.

The DaisySP submodule must be present (the engine's FX depend on it):

```bash
git submodule update --init lib/DaisySP    # run from the repo root
```

### Windows toolchain note

Rack plugins are native GCC/MinGW builds, so you need an **x86_64 MinGW-w64**
compiler (e.g. [WinLibs](https://winlibs.com/), or MSYS2's
`mingw-w64-x86_64-gcc`) — the MSVCRT variant matches Rack 2. Point `make` at it
with `CC=gcc CXX=g++` on the MinGW `bin/` in `PATH`.

Build from an **MSYS2 shell** (`pacman -S make jq`). This Makefile lists the
shared engine via absolute `$(REPO)/…` paths so the `.o` files stay in `build/`;
a *native* `make`/`mingw32-make` then trips over the `C:` drive colon
("multiple target patterns"). An MSYS2 `make` sees the paths as `/c/…` and
builds cleanly.

## Regenerating the panel

`generated_panel.hpp` and `Spotymod.svg` are both produced from one script so
the graphics and the widget positions can never drift:

```bash
python3 res/gen_panel.py     # run from host/vcv/
```

Edit the control table in `res/gen_panel.py`, re-run, rebuild.

## I/O

| Port | Meaning |
|------|---------|
| IN L/R | audio in (feeds the per-part FX chain; optional) |
| CLOCK | one pulse per beat → sets tempo (overrides the TEMPO knob) |
| RESET | reserved |
| OUT L/R | main mix |
| PIT A/B | per-part pitch modulation CV (±5 V, not strict V/Oct) |
| GAT A/B | per-part gate (10 V) |

## Verifying against the reference engine

The engine is unchanged, so its regression suite still applies:

```bash
cmake -S . -B build -G Ninja && cmake --build build   # from repo root
ctest --test-dir build --output-on-failure
```

For an A/B sound check, render a known scenario with the desktop host and
compare by ear against the module at identical parameter settings:

```bash
./build/render host/render/scenarios/demo_step_melody.json out.wav mods.csv
```
