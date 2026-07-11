# Spotykach — Modulation-First Firmware

Alternative firmware for the [Spotykach](https://synthux.academy/store/spotykach)
hardware, built around a single idea: **modulation is the instrument**. Two
symmetric parts, each driven by a performable modulation engine, feeding a
selectable sound source.

This is a fork of [Synthux-Academy/Spotykach](https://github.com/Synthux-Academy/Spotykach)
(the official firmware). It reuses the original hardware drivers, clocking and
bootloader, and replaces the instrument core with a new modulation-first design.

> **⚠️ Not yet tested on real hardware.** The modulation engine currently exists
> only as a portable C++ core, verified with the desktop **offline renderer**
> (unit tests + audio/CV render). The firmware shell that runs it on the Daisy
> arrives in milestone M6 — until then, nothing here has been flashed to or run
> on an actual Spotykach device.

## What makes this fork different

The stock firmware is a granular sampler you modulate. This fork inverts that:
the **modulation system is the primary interface**, and the sound engine is
whatever you point it at.

Each of the two **parts** is a **SuperModulator** — one performable macro
surface (RATE, SHAPE, PROBABILITY, SMOOTH, RANGE, DEPTH) sitting on top of
**five independent modulation lanes**, one per target. Every lane has its own
phase, its own random stream and its own probability dice, running at a fixed
musical ratio of the master rate. Shared character, independent motion: the
melody can rise while the filter falls. (A single output driving all targets
would just move everything in unison — a tremolo, not an instrument.)

Each lane can run as a smooth LFO (**FLOW**), a stepped sequence (**STEP**), or
slowly mutate over time (**EVOLVE**). A center section — **MORPH / COUPLE /
DRIFT / SPOT** — makes the interaction between the two parts playable, and CV +
gate outputs extend the modulation to the rest of the rack.

The full design intent lives in the residency's design spec; this README is a
self-contained summary of it.

## Architecture at a glance

One portable engine core, two hosts. No hardware type ever crosses into
`engine/`, so the exact same code runs in the desktop renderer and (later) on
the Daisy.

```
Spotykach/
├── engine/            portable instrument core — no libDaisy, no heap
│   ├── mod/           SuperModulator + the five modulation lanes
│   ├── parts/         Part, engine interface, sound engines
│   └── util/          math / DSP helpers
├── host/
│   └── render/        desktop CLI: scenario JSON → WAV + CSV
├── tests/             desktop unit tests for engine/ (doctest)
├── src/               original firmware (kept as reference for the M6 shell)
└── lib/               libDaisy + DaisySP (git submodules)
```

`Instrument` (`engine/instrument.h`) is the complete public API: `init(sample_rate)`,
normalized `0..1` parameter setters, and `process(in, out, size)`.

## Try it on the desktop (no hardware)

The engine is fully testable offline. You need a host C++ toolchain
(**clang** or gcc), **CMake**, and **Ninja**. doctest and nlohmann/json are
vendored under `third_party/`, so no test dependencies are fetched.

```bash
# optional: source a local env.sh to put your toolchain on PATH and set CC/CXX
source env.sh

cmake -S . -B build
cmake --build build
```

Run the unit tests:

```bash
ctest --test-dir build --output-on-failure
```

Render a scenario to audio + a modulation trace. A scenario is a JSON timeline
of parameter changes (see `host/render/scenarios/`):

```bash
./build/render.exe host/render/scenarios/melody_then_drift.json out.wav mods.csv
```

This writes:

- **`out.wav`** — the rendered audio.
- **`mods.csv`** — every lane's output plus pitch CV / gate per part, decimated
  for plotting. Ideal for *seeing* what FLOW / STEP / EVOLVE actually do.

`melody_then_drift` is a good starting point: a fixed 16-step melody that loops
identically, then slowly mutates as EVOLVE is dialled in.

## Roadmap

| Milestone | Scope | Status |
|-----------|-------|--------|
| **M1** | Portable engine foundation: SuperModulator, five lanes, `Instrument` API, desktop render host + tests | ✅ done |
| **M2** | Polyphonic synth voice (replaces the M1 test tone) | planned |
| **M3** | Capture sequencer (freeze the PITCH lane into a loop) | planned |
| **M4** | Center section — MORPH / COUPLE / DRIFT / SPOT | planned |
| **M5** | Sampler engine adapter (granular Deck/Vox) | planned |
| **M6** | Firmware shell: pads, gestures, panel, LEDs — runs on real hardware | planned |

## Hardware (upstream firmware — arrives in M6)

> The build/flash steps below compile and flash the **original upstream
> firmware**, not the modulation-first engine. The new firmware shell that
> hosts `engine/` on the Daisy is milestone **M6** and is not wired up yet.

### Setup

Clone recursively, or run `git submodule update --init --recursive` to fetch the
submodules (libDaisy + DaisySP).

Note: the ws2812 driver requires a slight modification to libDaisy, so the
libDaisy submodule points at a specific branch within the bleeptools fork (based
on the Infrasonic Audio fork), which also carries a few MIDI and mpr121 changes.

### Compiling

Build the libraries once (a `Makefile` target is provided):

```bash
make -j8 libs
```

Then build the firmware:

```bash
make -j8
```

On success the binaries land in `build/`: `spotykach.bin` (flashed via DFU) and
`spotykach.elf` (for debugging).

### Flashing

The bootloader enables USB DFU updating from the **external** USB-C port on the
rear of the main PCB (not the one on the Seed).

1. Compile the firmware (above).
2. Connect the main PCB's USB-C port to the computer (a data-capable cable).
3. Hold `Reset` on the back of the unit for ~3 seconds — the bottom-pad LEDs
   start to "breathe" in white.
4. Run `make program-dfu`.

The device then boots the new firmware. A bad flash can temporarily "brick" the
unit and require reinstalling the bootloader, firmware, or both.

## License & credits

MIT — see [`LICENSE`](LICENSE) (Copyright © 2026 Synthux Academy). Bundled and
submodule dependencies are documented in [`THIRD_PARTY.md`](THIRD_PARTY.md).
Original firmware credits are in [`CREDITS.md`](CREDITS.md).
