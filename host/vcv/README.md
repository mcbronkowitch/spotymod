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

The visual language stays "Spotykach" (dark plate, mint accents); a faithful
animated LED-ring widget (mirroring `src/ui/led.ring.h`) is a later polish step.

## Build

Requires the [VCV Rack SDK](https://vcvrack.com/manual/Building#Setting-up-the-Rack-SDK)
(matching your Rack major version, v2).

```bash
# from this directory:
RACK_DIR=/path/to/Rack-SDK make
RACK_DIR=/path/to/Rack-SDK make install   # copies the plugin into Rack's user dir
```

The DaisySP submodule must be present (the engine's FX depend on it):

```bash
git submodule update --init lib/DaisySP    # run from the repo root
```

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
./build/render host/render/scenarios/melody_then_drift.json out.wav mods.csv
```
