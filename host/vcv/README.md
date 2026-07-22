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

## Sampler

**ENG** (per part, latched) selects the part's engine: **Synth** or **Sampler**
— a granular texture deck over the shared record buffer. Flipping ENG to
Sampler on an empty part autoloads a factory sample (Bastian's own recording,
`res/factory.wav`) so the deck makes sound on the very first gesture; it never
overwrites content already in the buffer, and a deliberate *Clear sample*
stays cleared even if you flip ENG back and forth. On a Synth part, ENG is the
only mode control — REC is inert there.

**REC** (per part, latched) records from **IN L/R** into that part's buffer
while the sampler is free to keep playing what it already has (fill-follows:
the granular cloud reads whatever's been written so far, so the deck never
goes silent while filling). Starting a recording clears any remembered
sample path/factory flag — once REC has touched the buffer, its content no
longer matches a file on disk or the factory sample, so *Save sample…* is
the only way to keep it. The REC LED has three states: **pulsing** (2 Hz)
while recording, a **steady brightness proportional to fill level** once the
part holds content and isn't recording, and **dark** when the part is empty
or on Synth — the light tracks ENG, not leftover buffer state, so switching
a part away from Sampler doesn't relight it.

**Four Synth controls take on a different job the moment ENG says Sampler.**
The parameter IDs don't change — for the hardware this is a merge of existing
controls, not a new set of them — only what turning the knob does:

| Control | Label | Sampler meaning | Range |
|---|---|---|---|
| MELODY | `MELO` / `SCAN` | tape-head advance | centre is a true dead zone; exponential out to real time at three-quarters of travel, then linear up to 8×; sign is direction |
| DENSITY | `DENS` | grain overlap | 1…8, continuous; the MOTION lane modulates around it |
| SUB | `SUB` / `LEN` | grain length | 1 ms…42 s |
| DETUNE | `DTUN` / `ORG` | read position in the material | full material length |

SCAN's dead zone is exact and deliberate: a frozen tape head has to stay
frozen even through knob noise, so nothing moves for the first couple of
percent off centre. From there it ramps in gently, and real time lands on a
fixed, refindable knob position three-quarters of the way out rather than
somewhere you have to hunt for — the last quarter is the steepest stretch of
the curve, carrying the head up to eight times real time in either direction.

**SCAN springt beim ENG-Flip sofort auf die Knopfposition — offene Frage
(F-07, Review 2026-07-22).** MELO trägt im Synth VARIATION und im Sampler
SCAN, und die Init-Werte stehen für VARIATION an den Extremen (−0.728 und
−1.0). Als SCAN gelesen sind das −0.81× und −8× Realtime rückwärts: der
erste Flip auf Sampler lädt die Factory-Drone und schickt den Lesekopf im
selben Control-Tick rückwärts los, ohne dass jemand etwas angefasst hat.

Das ist als Fehler gemeldet worden, ist aber genau das Verhalten, das
"Known limitations" weiter unten bewusst wählt: Knopfposition gilt über den
Engine-Wechsel hinweg, ohne getrenntes Gedächtnis und ohne Soft-Takeover,
weil die Hardware kein Soft-Takeover hat. Ein Soft-Takeover wurde gebaut und
wieder zurückgenommen — über Patch-Laden hinweg dicht zu bekommen verlangt
persistenten Zustand, also genau das, was diese Zeile ausschließt. Die
Entscheidung liegt beim Autor des Instruments, nicht in der Engine.

Mit derselben Änderung ging eine stille Last weg (K-03): `sampler_scan()`
wurde für **beide** Decks aufgerufen, auch für ein Synth-Deck, und
`scan_rate()` enthält im Exponentialast ein `std::pow`. Bei `ctrlDiv = 16`
waren das bis zu 6000 `pow`-Aufrufe pro Sekunde im Audio-Callback für eine
Engine, die niemand hört. Der Aufruf hängt jetzt an `samplerPart`, wie SUB
und DTUN es schon taten.

**NEW and TRIG both fire "new grain now" in the Sampler:** the tape head
snaps back to ORGANIZE's position and a fresh grain spawns immediately. This
exists because a grain's position, pitch and length are frozen the instant
it's spawned, and the next chance to change any of them is the next
scheduled spawn — at overlap 1 and a long LEN that's up to ten seconds away.
Without this gesture, the long end of LEN wouldn't be a playable state at
all; the deck would just stop answering every knob for that stretch. On
TRIG this comes on top of the ordinary trigger, not instead of it.

**The tape head shows up on the LED ring** as a bright travelling dot,
as soon as a part is in Sampler and has material in its buffer.

**Pitch holds still in the Sampler.** The PITCH lane is switched off there;
tuning happens exclusively through TUNE, the bipolar ±18-semitone transpose
shared with the Synth's scale grid. The point is that a Dorian sample on one
deck and a Dorian-played Synth on the other land in the same key. Rhythmic
triggering through STEP survives this untouched — the lane keeps firing on
step boundaries, it just stops moving pitch while it does.

**SUB and DTUN give up their Synth jobs in the Sampler.** They no longer
reach it as octave share and detune — those two abilities are retired here
so that a single knob doesn't carry two jobs inside the same engine. The
Synth keeps both.

The right-click context menu carries a **Sampler A / Sampler B** submenu per
part:
- **Load sample…** / **Save sample…** — WAV import/export via a file dialog.
- **Clear sample** — empties the buffer and forgets any remembered
  path/factory flag.
- **Speed mode** — Tape (default) or Digital. Tape couples speed and pitch
  like varispeed; Digital repitches grains at unchanged grain duration.
- **Reverse** — plays the buffer backwards.
- **Overdub feedback** — how much of the existing buffer content survives
  under a new recording (a slider, not a toggle).
- **Engine: test tone (dev)** — a leftover development aid; with it set,
  ENG's second position plays a test tone instead of the sampler. Not meant
  for normal patches.

A recorded or loaded texture **survives patch save/reopen**, but not through
`dataToJson`/`dataFromJson` — those only carry the sample path, speed mode,
reverse, feedback and a couple of internal flags (`factory`, `factoryTried`).
`factoryTried` in particular is what makes a deliberate *Clear sample* stay
cleared through save/reopen: it is persisted intent, restored verbatim from
JSON, and nothing after that point overwrites it back to false — only Rack
*Initialize* (`onReset`) resets it, letting the factory sample autoload
again.
The audio itself goes through Rack's **patch storage** instead: `onSave`
writes any part that didn't come from a file or the factory WAV out as a WAV
into the patch's own storage directory, and `dataFromJson`/`onAdd` reload it
from there on reopen (a part whose content DID come from a file or the
factory sample reloads from that source instead, so nothing is written for
it). Every part that this save does *not* write its own stored WAV for —
because it now has a file path, is factory-loaded, or has nothing recorded —
also has any leftover stored WAV from an earlier save deleted, so neither a
deliberate *Clear sample* nor loading a file over an old recording leaves a
stale WAV sitting in patch storage.

### Known limitations

- **Knob position holds across engines — there is no separate memory and no
  soft takeover.** This is on purpose: it's the one behaviour VCV and the
  eventual hardware can share exactly, since the hardware has no
  soft-takeover to fall back on. The price is that an ENG switch can't be
  prepared in advance. Right up until the last second, these four knobs are
  still the Synth's knobs — dialing in SUB ahead of time audibly detunes the
  Synth that's still playing. Every switch forces the sequence "wrong first,
  then dial it in." Fine for a staged transition on stage; not for a
  seamless one.
- **There are no parameter CV inputs.** The jacks are IN L/R, CLOCK and
  RESET; PIT and GAT are outputs. External modulation of these controls only
  reaches VCV through third-party mapping modules.
- **A sample-rate DROP silently truncates the recording's tail.** The record
  buffer is sized in frames (42 s × the engine's sample rate), so switching
  your audio device from 48 kHz down to 44.1 kHz shrinks that allocation and
  loses roughly the last 1.2 s of a full 42 s buffer. This is safe — the
  engine clamps every read to the smaller capacity — but nothing in the UI
  warns you it happened.
- **A sample-rate CHANGE does not resample buffer content, but a file LOAD
  does.** If you already have material recorded or loaded into a part and
  then change your audio device's rate, that buffer plays back transposed at
  the new rate — this is deliberate tape behaviour, the same as changing tape
  speed. A fresh *Load sample…*, by contrast, always resamples the WAV to
  the engine's current rate before writing it into the buffer, so an
  imported file is always in tune. The asymmetry is intentional: importing a
  file at the wrong pitch would be a bug, but re-rating material that's
  already sitting in the buffer is varispeed, not a bug.
- **Memory:** each `Spotymod` instance allocates well over its two 42 s
  stereo record buffers up front, whether or not the sampler is ever used on
  either part — closer to **42 MB total**, not the 32 MB the record buffers
  alone account for:
  - ~32 MB — the two 42 s stereo sampler record buffers (`samplerMem`).
  - ~4.19 MB — the per-part stereo echo buffers the FX chain requires
    (`echo[]`: `2 × 2 × 262144` floats = 4,194,304 B), unrelated to the
    sampler.
  - ~130 KB — the shared reverb (`AmbientReverb`).
  - ~6.4 MB — the factory sample cache (`factoryNative`, decoded once in
    `onAdd()`, plus the rate-converted `factoryL`/`factoryR`, rebuilt in
    `reinit()`), held for the module's lifetime regardless of whether ENG
    is ever flipped to Sampler on either part.

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
Restart Rack and the module appears under the **ton-k** brand
("Spotymod" in the module browser). A self-built plugin is unsigned, so Rack may
note it isn't from the library — it still loads.

**Windows: check where `install` actually copied to.** The SDK builds the
target path from `$(LOCALAPPDATA)`, and MSYS2/Git-Bash don't export that name
into the make environment — the variable expands empty and the package lands in
`C:\Rack2\plugins-win-x64\` instead, with `make` reporting success either way.
Pass the directory explicitly:

```bash
make install RACK_USER_DIR="$LOCALAPPDATA/Rack2"
```

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

If you build from a **Git-Bash** shell instead of a real MSYS2 login shell, gcc
may die with `Cannot create temporary file … Permission denied` — the recipe
shell inherits `TMP=C:\WINDOWS`. Pass a writable temp dir and a POSIX shell:

```bash
make CC=gcc CXX=g++ SHELL=/usr/bin/bash \
  TMP="$LOCALAPPDATA/Temp" TEMP="$LOCALAPPDATA/Temp" -j4
```

From a normal MSYS2 shell you can drop those overrides.

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
| IN L/R | audio in (feeds the per-part FX chain and, while REC is latched on a Sampler part, that part's record buffer; optional) |
| CLOCK | one pulse per beat → sets tempo (overrides the TEMPO knob) and phase-aligns the transport on each pulse |
| RESET | resets the transport downbeat (bar/beat phase) |
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
