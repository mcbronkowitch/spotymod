# M5b — Sampler on the VCV Panel: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the M5a texture deck reachable and playable from the VCV Rack
plugin — ENG selects Synth ↔ Sampler, a REC pad records from IN L/R, the
context menu carries load/save/clear and the edit layer, a factory sample
autoloads so one pad press makes sound, and everything survives patch
save/reopen.

**Architecture:** M5b is **host-only work with one small engine addition.**
The engine API is complete (`engine/instrument.h:95-112`), and the voice row
already routes into both engines at `Part` level
(`engine/parts/part.h:81-86`) — ATK/DEC/FILT/RES/SUB/DTUN reach the cloud with
no host change. What is missing: the VCV module never allocates sampler memory
(`fxmem.sampler_buf` stays `nullptr`), `ENGINE_SAMPLER` is not selectable, and
the module has no `dataToJson`/`dataFromJson` at all.

**Tech Stack:** C++17, VCV Rack 2 SDK (2.6.6), MSYS2 `make` + WinLibs mingw,
Python 3 for the panel generator, doctest for engine tests.

## Global Constraints

Every task's requirements implicitly include this section.

- **Never `git add -A`.** Stage explicit paths only.
- Commit trailer on every commit:
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **Never modify `host/vcv/plugin.json`** — it stays at version `2.7.0`.
  **Never create a `v*` git tag.** Releases are the human's call.
- **Never push.** The branch stays local.
- `src/` is the frozen original firmware reference — **never modify it.**
- **No heap allocation in `engine/`**; no allocation, no `std::` math, no
  `assert()` on per-sample paths; `engine/` must never include libDaisy.
  Host code (`host/`) may allocate freely, off the audio thread.
- **`PART_STRIDE` stays 23.** New panel params are appended at the END of
  `PARAMS` in `res/gen_panel.py`, never inserted into `part_controls()`.
  Every existing param id must keep its value or every saved `.vcv` breaks.
- `res/test_panel.py`'s `PARAM_ORDER` / `LIGHT_ORDER` are a **frozen
  contract, extended by hand** — the test compares them against the
  generator's output; it is not derived from it.
- **Synth neutrality:** a part on `ENGINE_SYNTH` must still render
  bit-identically. No task may change engine behaviour on the synth path.
- **Determinism:** a double render of the same scenario stays byte-identical.
- Engine tests are doctest, run via the desktop build
  (`docs/` build notes; `CMAKE_BUILD_TYPE` must match between compared runs —
  a Debug-vs-Release mismatch produces a false neutrality alarm).
- The panel must stay reducible to the real hardware: **REC is the only new
  panel element.** Everything else goes in the context menu.

## Spec

`docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md`,
section **VCV layer** (lines 254-286) and **Material: recording, loading,
persistence** (lines 213-252).

### Two deliberate deviations from the spec

Both are decided here; the implementer follows the plan, not the spec text.

1. **WAV I/O: reuse the repo's own reader/writer, do not vendor `dr_wav`.**
   The spec (written 2026-07-18) calls for a vendored `dr_wav`. Since then
   M5a shipped `host/render/wav_reader.h` and `wav_writer.h` — hand-rolled,
   hardened (every declared chunk size is validated against the actual
   remaining file bytes before allocating or seeking), covered by
   `tests/test_wav.cpp`, and already handling 16/24/32-bit PCM, 32-bit float
   and `WAVE_FORMAT_EXTENSIBLE`. Vendoring a second WAV implementation would
   add a dependency and a second code path for no gain. Task 2 moves the two
   headers to `host/shared/` so both hosts can include them without one host
   depending on the other.

2. **Sample-rate change: the host snapshots the audio and pushes it back.**
   The spec does not address it; the M5a findings flag it as a real defect
   (`reinit()` runs on every sample-rate change and would discard a loaded
   sample).

   **Corrected 2026-07-21, after the first Task 1 attempt failed its own
   test.** This plan originally claimed `init()` only resets the buffer's
   indices, so remembering `rec_size()` and re-declaring it afterwards would
   be enough — no copy, no allocation. **That premise is false.**
   `SampleBuffer::init()` ends in `clear()` (`sample_buffer.cpp:33`), and
   `clear()` `memset`s the whole injected buffer (`sample_buffer.cpp:179-180`).
   There is no audio left to re-declare. The implementer hit this as a
   failing assertion and stopped rather than patch `clear()`'s semantics —
   which would have been an audio-path change to make a plan's wrong
   assumption true.

   So the host copies: read the content out through `sample_data()` before
   `reinit()`, then push it back with the existing `load_sample()`. That
   costs one temporary allocation and two copies of up to 16 MB per part, on
   the main thread, on an event that happens when a user changes their
   audio device. `restore()` is therefore **not** part of this plan.

   Consequence, accepted and documented: the snapshot is **not resampled**,
   so a texture recorded at 44.1 kHz plays back transposed after a switch to
   48 kHz. That is tape behaviour and matches the instrument's idiom. Note
   this is the opposite of what happens on *file load*, where Task 6 does
   resample — importing a file at the wrong pitch is an import bug, while
   re-rating material already in the buffer is varispeed.

## File Structure

**Created:**

| File | Responsibility |
|------|----------------|
| `host/shared/wav_reader.h` | moved from `host/render/` — RIFF reader, both hosts |
| `host/shared/wav_writer.h` | moved from `host/render/` — RIFF writer, both hosts |
| `host/vcv/src/sampler_ui.hpp` | VCV-side sampler glue: per-part host state struct, WAV load/save helpers, context-menu builder. Keeps `Spotymod.cpp` from growing past ~700 lines. |
| `host/vcv/res/factory.wav` | factory sample (bounced spotymod drone), ~6 s stereo |
(`host/vcv/res/factory.wav` is a copy of a file Bastian supplied, not a
generated asset — see Task 8.)

**Modified:**

| File | Change |
|------|--------|
| `engine/sampler/sampler_engine.h` | `+ sample_data()` |
| `engine/instrument.h` | `+ sampler_data()`, `+ sampler_rec_size()`, `+ sampler_is_recording()` |
| `host/render/main.cpp`, `host/render/scenario.cpp`, `tests/test_wav.cpp` | include path `render/wav_*.h` → `shared/wav_*.h` |
| `host/vcv/res/gen_panel.py` | REC pad pair appended to `PARAMS`; REC light pair appended to `LIGHTS`; PLAY-row x-coordinates re-spaced |
| `host/vcv/res/test_panel.py` | `PARAM_ORDER` + `LIGHT_ORDER` extended; new `test_rec_params` |
| `host/vcv/src/generated_panel.hpp` | regenerated (never edited by hand) |
| `host/vcv/res/Spotymod.svg` | regenerated |
| `host/vcv/src/Spotymod.cpp` | sampler memory, ENGINE remap, REC wiring, persistence, factory autoload |
| `host/vcv/Makefile` | `-I$(REPO)/host` for the shared WAV headers |
| `host/vcv/README.md` | the new panel element and the menu |

---

## Task 1: Engine readback

One engine-side hook: read the recorded content back out, for *Save sample…*,
for the patch-storage autosave, and for the sample-rate snapshot. Thin —
`SampleBuffer` already has `raw()` and `rec_size()` public
(`engine/sampler/sample_buffer.h:44,52`).

**This task was attempted once and correctly refused.** The original version
also added a `restore(size_t)` that re-declared the content length after
`init()`, on the premise that `init()` leaves the audio in place. It does
not: `SampleBuffer::init()` ends in `clear()` (`sample_buffer.cpp:33`) and
`clear()` `memset`s the whole buffer (`sample_buffer.cpp:179-180`). The
implementer hit that as a failing assertion and stopped instead of loosening
`clear()`'s semantics to make the plan true. `restore()` is gone; Task 4
copies instead. **Do not reintroduce it, and do not modify `clear()` or
`init()`.**

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (public section, after `load_sample`)
- Modify: `engine/instrument.h` (after `load_sample`, line 110-112)
- Modify: `engine/parts/part.h` (a const `sampler()` overload, if absent)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: `SampleBuffer::raw()`, `rec_size()`
- Produces:
  - `const SampleBuffer::Frame* SamplerEngine::sample_data() const`
  - `const SampleBuffer::Frame* Instrument::sampler_data(int p) const`
  - `size_t Instrument::sampler_rec_size(int p) const`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sampler_engine.cpp`:

```cpp
TEST_CASE("sample_data exposes the loaded content at rec_size length") {
    std::vector<SampleBuffer::Frame> mem(4096);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(1);
    eng.init(48000.f);
    CHECK(eng.sample_data() == mem.data());   // the host's own pointer back
    CHECK(eng.rec_size() == 0);

    std::vector<float> l(1000), r(1000);
    for (size_t i = 0; i < 1000; ++i) {       // a ramp, so an offset shows up
        l[i] = (float)i / 1000.f;
        r[i] = -(float)i / 1000.f;
    }
    eng.load_sample(l.data(), r.data(), 1000);
    CHECK(eng.rec_size() == 1000);
    CHECK(eng.sample_data()[0].l   == doctest::Approx(0.f));
    CHECK(eng.sample_data()[500].l == doctest::Approx(0.5f));
    CHECK(eng.sample_data()[500].r == doctest::Approx(-0.5f));
    CHECK(eng.sample_data()[999].l == doctest::Approx(0.999f));
}

TEST_CASE("a host can carry content across a re-init by copying it out") {
    // This is exactly what the VCV host does on a sample-rate change, and the
    // reason sample_data() exists. init() memsets the injected buffer
    // (SampleBuffer::clear), so the snapshot MUST be taken into separate
    // storage first -- copying out of the buffer after init() would read
    // zeroes, and that mistake is what this case is here to catch.
    std::vector<SampleBuffer::Frame> mem(4096);
    SamplerEngine eng;
    eng.set_memory(mem.data(), mem.size());
    eng.set_seed(1);
    eng.init(48000.f);

    std::vector<float> l(800, 0.25f), r(800, -0.75f);
    eng.load_sample(l.data(), r.data(), 800);

    const size_t n = eng.rec_size();
    std::vector<float> sl(n), sr(n);
    for (size_t i = 0; i < n; ++i) { sl[i] = eng.sample_data()[i].l;
                                     sr[i] = eng.sample_data()[i].r; }

    eng.init(44100.f);
    CHECK(eng.rec_size() == 0);
    CHECK(eng.sample_data()[10].l == doctest::Approx(0.f));  // init DID wipe it

    eng.load_sample(sl.data(), sr.data(), n);
    CHECK(eng.rec_size() == 800);
    CHECK(!eng.is_empty());
    CHECK(eng.sample_data()[10].l  == doctest::Approx(0.25f));
    CHECK(eng.sample_data()[799].r == doctest::Approx(-0.75f));
}

TEST_CASE("sample_data is null without injected memory") {
    SamplerEngine bare;
    bare.set_seed(1);
    bare.init(48000.f);
    CHECK(bare.sample_data() == nullptr);
    CHECK(bare.rec_size() == 0);
    CHECK(bare.is_empty());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run (from the repo root, desktop build configured as in the build notes):

```bash
cmake --build build --target spky_tests && ./build/spky_tests
```

Expected: FAIL — `'class spky::SamplerEngine' has no member named 'sample_data'`.

- [ ] **Step 3: Add the engine accessor**

In `engine/sampler/sampler_engine.h`, directly after
`void load_sample(const float* l, const float* r, size_t frames);`:

```cpp
    // Read the recorded content back out: Save sample..., the patch-storage
    // autosave, and the host's sample-rate snapshot all need it. This hands
    // back the very pointer the host injected, so the caller must respect
    // rec_size() as the valid length -- past it lie stale frames. nullptr
    // when no memory was injected.
    //
    // Note for callers carrying content across an init(): copy OUT first.
    // init() ends in clear(), which memsets this whole buffer.
    const SampleBuffer::Frame* sample_data() const { return _buf.raw(); }
```

- [ ] **Step 4: Add the Instrument facade**

In `engine/instrument.h`, after the closing brace of `load_sample`
(line 112):

```cpp
    const SampleBuffer::Frame* sampler_data(int p) const {
        return _parts[p].sampler().sample_data();
    }
    size_t sampler_rec_size(int p) const { return _parts[p].sampler().rec_size(); }
```

`Part::sampler()` must have a const overload for `sampler_data`/`sampler_rec_size`
to compile — check `engine/parts/part.h`; if only a non-const `sampler()`
exists, add `const SamplerEngine& sampler() const { return _sampler; }`
beside it.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build --target spky_tests && ./build/spky_tests -ts="*"
```

Expected: PASS, and the whole suite still green (481 tests before this task).

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/instrument.h engine/parts/part.h tests/test_sampler_engine.cpp
git commit -m "feat(sampler): expose the record buffer for readback

The hook the VCV host needs for Save sample..., the patch-storage autosave,
and carrying content across a sample-rate change.

An earlier draft of this also added a restore() that re-declared the content
length after init(), on the assumption that init() leaves the audio in place.
It does not -- init() ends in clear(), which memsets the whole injected
buffer. Hosts must copy the content out before re-initialising; the test case
pins exactly that, including the wipe itself.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 2: Move the WAV headers to `host/shared/`

Both hosts need WAV I/O. Today the reader and writer live under
`host/render/` and are included as `"render/wav_reader.h"` with `-I host`.
Including them from `host/vcv` would make one host depend on another; move
them to a neutral home instead.

**Files:**
- Move: `host/render/wav_reader.h` → `host/shared/wav_reader.h`
- Move: `host/render/wav_writer.h` → `host/shared/wav_writer.h`
- Modify: `host/render/main.cpp:7-8`, `host/render/scenario.cpp:7`,
  `tests/test_wav.cpp:5-6`
- Modify: `CMakeLists.txt` (if it lists the headers explicitly)

**Interfaces:**
- Produces: `#include "shared/wav_reader.h"` / `#include "shared/wav_writer.h"`,
  reachable from any host with `-I <repo>/host`. The API is unchanged:
  `spky::WavData { int sample_rate; std::vector<float> l, r; }`,
  `bool spky::read_wav(const std::string& path, WavData& out, std::string& err)`,
  and the writer's counterpart.

- [ ] **Step 1: Move the files with git**

```bash
mkdir -p host/shared
git mv host/render/wav_reader.h host/shared/wav_reader.h
git mv host/render/wav_writer.h host/shared/wav_writer.h
```

- [ ] **Step 2: Update the three include sites**

`host/render/main.cpp` lines 7-8:

```cpp
#include "shared/wav_reader.h"
#include "shared/wav_writer.h"
```

`host/render/scenario.cpp` line 7:

```cpp
#include "shared/wav_reader.h"
```

`tests/test_wav.cpp` lines 5-6:

```cpp
#include "shared/wav_writer.h"
#include "shared/wav_reader.h"
```

- [ ] **Step 3: Fix the build files**

Check `CMakeLists.txt` for any explicit reference to the old paths
(`grep -n "wav_" CMakeLists.txt`). The include directory is `host/`, which
does not change. Update only what actually names the old path.

- [ ] **Step 4: Build and run the full suite**

```bash
cmake --build build --target spky_tests && ./build/spky_tests -ts="*"
cmake --build build --target spky_render
```

Expected: PASS, and `spky_render` links. `tests/test_wav.cpp` already covers
the round-trip — this task must not change its assertions, only its includes.

- [ ] **Step 5: Commit**

```bash
git add host/shared host/render/main.cpp host/render/scenario.cpp tests/test_wav.cpp CMakeLists.txt
git commit -m "refactor(host): move the WAV reader/writer to host/shared

The VCV plugin needs the same WAV I/O the render host has. Sharing them from
host/render would make one host depend on another; host/shared is the neutral
home. No behaviour change -- includes only.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 3: Panel — the REC pad and its LED

The **only** new panel element. Appended at the end of `PARAMS` and `LIGHTS`
so every existing id keeps its value and `PART_STRIDE` stays 23.

Placement: the PLAY row's left block currently holds ENG (11.5) and GRIT
(22.0), then STEPS (35.5), then STEP (46.0). Re-space the left block to fit
REC and its LED. Glyph radii: `LATCH` 2.7, `KNOBI` 3.0, `LIGHT` 1.7
(`gen_panel.py:63-64`); `test_no_overlap` requires centre distance ≥ the sum
of the two radii for every params/inputs/outputs pair.

Chosen coordinates (part A; B mirrors via `W - x`, `W = 213.36`):

| element | x | nearest neighbour | distance | required |
|---|---|---|---|---|
| ENG (LATCH) | 10.0 | GRIT | 7.5 | 5.4 |
| GRIT (LATCH) | 17.5 | REC | 7.5 | 5.4 |
| REC (LATCH) | 25.0 | STEPS | 12.0 | 5.7 |
| REC LED (LIGHT) | 30.0 | — | — | not in the overlap test |
| STEPS (KNOBI) | 37.0 | STEP | 9.0 | 5.7 |
| STEP (LATCH) | 46.0 | unchanged | | |

**Files:**
- Modify: `host/vcv/res/gen_panel.py` (lines 162-163, 197-201, and the
  `PARAMS` tail at 349-352, and the `LIGHTS` list at 360-361)
- Modify: `host/vcv/res/test_panel.py` (`PARAM_ORDER`, `LIGHT_ORDER`, new test)
- Regenerate: `host/vcv/src/generated_panel.hpp`, `host/vcv/res/Spotymod.svg`

**Interfaces:**
- Produces: `ParamId` gains `REC_A`, `REC_B` (ids 78, 79 — the new
  `NUM_PARAMS` is 82); `LightId` gains `REC_A_L`, `REC_B_L` (ids 2, 3 —
  `NUM_LIGHTS` becomes 4). `kLightCtls[0]` and `[1]` stay the gate lights, so
  `SpotymodWidget`'s LED-ring centring (`Spotymod.cpp:509`) is unaffected.

- [ ] **Step 1: Extend the frozen contract in `test_panel.py` (the failing test first)**

In `res/test_panel.py`, append `'REC_A', 'REC_B',` to the end of
`PARAM_ORDER` (after `'ROT_A', 'ROT_B',` on line 50) and rewrite
`LIGHT_ORDER` (line 54):

```python
LIGHT_ORDER = ['GATE_A_L', 'GATE_B_L', 'REC_A_L', 'REC_B_L']
```

And add a new test after `test_dust_rot_kind`:

```python
def test_rec_params():
    """REC is appended, not templated -- appending keeps PART_STRIDE at 23 so
    every saved .vcv keeps its param ids. Same guard shape as
    test_dust_params, and the kind is pinned the same way test_dust_rot_kind
    pins DUST/ROT: a LATCH that silently became an SMBTN would still clear
    test_no_overlap (identical radius), so the kind needs its own check."""
    check(g.PART_STRIDE == 23, "PART_STRIDE must stay 23")
    ids = {c.enum: i for i, c in enumerate(g.PARAMS)}
    for e in ("REC_A", "REC_B"):
        check(e in ids, f"{e} missing")
        check(ids[e] >= 2 * g.PART_STRIDE, f"{e} must be appended, not templated")
    check(ids["REC_A"] > ids["ROT_B"], "REC must append AFTER the existing tail")
    h = g.header()
    for e in ("REC_A", "REC_B"):
        check(h.count(f"{{{e}, WK_LATCH,") == 1,
              f"{e} is not WK_LATCH in the generated header")
```

- [ ] **Step 2: Run the panel tests to verify they fail**

```bash
python host/vcv/res/test_panel.py
```

Expected: FAIL — `PARAMS order changed` (the generator does not emit REC yet).

- [ ] **Step 3: Re-space the PLAY row and append REC**

In `res/gen_panel.py`, replace lines 162-163:

```python
# The PLAY row's left block re-spaced to seat REC between GRIT and STEPS
# (spec 2026-07-18 "VCV layer": REC is the only new panel element). All four
# left-block glyphs are LATCH r=2.7 except STEPS (KNOBI r=3.0); the pitches
# below clear test_no_overlap's radius-sum minimum with >=1.8 mm to spare.
PAD_X    = [10.0, 17.5, 46.0, 56.5, 67.0, 77.5]   # ENG GRIT | STEP PRIN NEW TRIG
STEPS_X  = 37.0                     # sequencer knob, between the two pad blocks
REC_X    = 25.0                     # REC pad (appended param, not templated)
REC_LED_X = 30.0                    # its state LED, right of the pad
```

Then append to the `PARAMS` list, after the `ROT_A`/`ROT_B` entries
(gen_panel.py:352):

```python
    # M5b: REC, the one new panel element of the texture deck. Appended LAST
    # so REC_A/REC_B take fresh trailing ids and PART_STRIDE stays 23 -- every
    # already-saved .vcv keeps every param id it has.
    Ctl("REC_A", LATCH, REC_X,     PLAY_Y, "REC"),
    Ctl("REC_B", LATCH, W - REC_X, PLAY_Y, "REC"),
```

And append to the `LIGHTS` list, after `GATE_B_L` (gen_panel.py:361):

```python
    # Appended after the gate lights: the C++ side centres the LED rings on
    # kLightCtls[0..1], so the gate lights must keep index 0 and 1.
    Ctl("REC_A_L", LIGHT, REC_LED_X,     PLAY_Y, ""),
    Ctl("REC_B_L", LIGHT, W - REC_LED_X, PLAY_Y, ""),
```

- [ ] **Step 4: Regenerate and verify**

```bash
python host/vcv/res/gen_panel.py
python host/vcv/res/test_panel.py
```

Expected: the generator rewrites `res/Spotymod.svg` and
`src/generated_panel.hpp`; all panel tests PASS, including `test_enum_order`,
`test_no_overlap` and the new `test_rec_params`.

Then confirm by reading `src/generated_panel.hpp` that `NUM_PARAMS` is 82,
`NUM_LIGHTS` is 4, and `PART_STRIDE` is still 23.

- [ ] **Step 5: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/test_panel.py host/vcv/res/Spotymod.svg host/vcv/src/generated_panel.hpp
git commit -m "feat(panel): add the REC pad and its LED per part

The only new panel element of the texture deck. Appended at the end of PARAMS
and LIGHTS so PART_STRIDE stays 23 and every saved patch keeps its param ids;
the PLAY row's left block is re-spaced to seat it between GRIT and STEPS.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 4: Sampler memory, ENGINE remap, sample-rate survival

Three changes that only make sense together: the sampler needs memory before
it can be selected, selecting it needs the ENGINE switch remapped, and both
are worthless if a sample-rate change wipes the content.

The module currently holds its FX memory **by value**
(`Spotymod.cpp:82-83`). The sampler buffers must **not** follow that pattern:
42 s stereo at 48 kHz is ~16 MB per part, ~32 MB for both, and Rack
constructs modules where a by-value member of that size is a liability. Use
heap `std::vector` members.

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp` (members ~82, constructor ~95-102,
  `reinit` 228-231, `pushParams` 276-277, `configControls` 149-150,
  `process` 318-319)
- Create: `host/vcv/src/sampler_ui.hpp` (the per-part host state struct only;
  Tasks 6-8 fill in the rest)

**Interfaces:**
- Consumes: `Instrument::sampler_rec_size(p)`, `sampler_data(p)`, `load_sample(...)`
  (Task 1); `FxMem::sampler_buf[]`, `FxMem::sampler_frames`.
- Produces: `struct SamplerPartState` in `sampler_ui.hpp`, one per part,
  holding everything the module must persist and the menu must edit:
  ```cpp
  struct SamplerPartState {
      std::string path;       // last file loaded, "" if none / live-recorded
      int   tapeIdx  = 1;     // 0 = Digital, 1 = Tape (varispeed, the default)
      bool  reverse  = false;
      float feedback = 0.95f; // overdub feedback, ~-3 dB
      bool  testTone = false; // dev: ENG's sampler slot plays the test tone
      bool  factoryLoaded = false;  // this part's content came from the factory WAV
  };
  ```
  `tapeIdx` is an `int`, not a `bool`, because Rack's
  `createIndexPtrSubmenuItem` (Task 6) writes an index — one representation
  of the fact, not two.

- [ ] **Step 1: Create the state header**

`host/vcv/src/sampler_ui.hpp`:

```cpp
#pragma once
#include <string>

// Host-side state of the M5b texture deck, one instance per part. Everything
// here is edit-layer state the engine does not own and the module must
// persist: the engine keeps the audio, this keeps how it got there.
namespace spkyvcv {

struct SamplerPartState {
    std::string path;       // last file loaded, "" if none / live-recorded
    int   tapeIdx  = 1;     // 0 = Digital, 1 = Tape -- an index, because the
                            // context menu binds it with createIndexPtrSubmenuItem
    bool  reverse  = false;
    float feedback = 0.95f; // overdub feedback, ~-3 dB (engine default)
    bool  testTone = false; // dev: ENG's sampler slot plays the test tone instead
    bool  factoryLoaded = false;  // content came from the factory WAV, not the user
};

}  // namespace spkyvcv
```

- [ ] **Step 2: Add the members and the allocation**

In `Spotymod.cpp`, add near the top with the other includes:

```cpp
#include <vector>
#include "sampler_ui.hpp"
```

Replace the FX-memory member block (after line 83) by adding:

```cpp
    // The texture deck's record buffers. Unlike the echo/reverb memory above
    // these are NOT held by value: 42 s of stereo at 48 kHz is ~16 MB per
    // part. The engine's "no heap" contract binds engine/, not the host --
    // hosts allocate (desktop: std::vector, M6 firmware: SDRAM).
    static constexpr double kSamplerBufferSeconds = 42.0;
    std::vector<spky::SampleBuffer::Frame> samplerMem[spky::PART_COUNT];
    spkyvcv::SamplerPartState smp[spky::PART_COUNT];
```

- [ ] **Step 3: Allocate in `reinit`, and restore the content length**

Replace `reinit` (lines 228-231) with:

```cpp
    // Re-init the engine for a new sample rate. Without the snapshot below,
    // every rate change silently discarded a loaded or recorded sample (an
    // M5a finding). Two things destroy it: the buffers are sized in FRAMES so
    // a rate change resizes them, and inst.init() ends in SampleBuffer::clear()
    // which memsets the whole buffer. So the content must be copied OUT into
    // separate storage first and pushed back afterwards -- reading it out of
    // the buffer after init() would read zeroes.
    //
    // Up to 16 MB per part, twice, but only when the user changes their audio
    // device, and onSampleRateChange runs on the main thread with the engine
    // paused. The snapshot is NOT resampled -- it plays transposed at the new
    // rate. That is varispeed, and it is the instrument's idiom. (File LOADS
    // do resample, see sampler_ui.hpp: importing at the wrong pitch is a bug,
    // re-rating material already in the buffer is tape.)
    void reinit(float sr) {
        std::vector<float> snapL[spky::PART_COUNT], snapR[spky::PART_COUNT];
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            const size_t n = inst.sampler_rec_size(p);
            const spky::SampleBuffer::Frame* f = inst.sampler_data(p);
            if (!n || !f) continue;
            snapL[p].resize(n);
            snapR[p].resize(n);
            for (size_t i = 0; i < n; ++i) { snapL[p][i] = f[i].l; snapR[p][i] = f[i].r; }
        }

        curSr = sr;
        const size_t frames = (size_t)(kSamplerBufferSeconds * (double)sr);
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            if (samplerMem[p].size() != frames) samplerMem[p].resize(frames);
            fxmem.sampler_buf[p] = samplerMem[p].data();
        }
        fxmem.sampler_frames = frames;

        inst.init(sr, fxmem);

        for (int p = 0; p < spky::PART_COUNT; ++p)
            if (!snapL[p].empty())
                inst.load_sample(p, snapL[p].data(), snapR[p].data(), snapL[p].size());
    }
```

- [ ] **Step 4: Move the re-init off the audio thread**

`process()` currently catches a rate change reactively
(`if (args.sampleRate != curSr) reinit(args.sampleRate);`, line 319). That
call now resizes ~32 MB. Add the proper Rack hook so the work happens on the
main thread with the engine paused, and keep the `process()` check as the
first-call path:

```cpp
    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        reinit(e.sampleRate);
    }
```

Leave line 319 as it is — once `onSampleRateChange` has run, `curSr` matches
and the check is a no-op; it still covers the very first `process()` call.

- [ ] **Step 5: Remap the ENGINE switch**

In `configControls` (lines 149-150), replace the labels:

```cpp
                    if (c.id == ENGINE_A || c.id == ENGINE_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Engine",
                                     {"Synth", "Sampler"});
```

In `pushParams` (lines 276-277), replace the engine selection:

```cpp
            // ENG picks Synth or Sampler. The test tone survives as a dev tool
            // in the context menu: with testTone set, ENG's second position
            // plays it instead of the sampler. A patch saved before M5b that
            // had "test tone" selected therefore opens as sampler -- accepted
            // (spec 2026-07-18 "VCV layer"), no real patch uses it.
            const bool eng2 = ppb(ENGINE_A, p);
            inst.set_engine(p, !eng2 ? spky::ENGINE_SYNTH
                                     : (smp[p].testTone ? spky::ENGINE_TEST_TONE
                                                        : spky::ENGINE_SAMPLER));
```

- [ ] **Step 6: Push the edit-layer state every control tick**

Still in `pushParams`, inside the per-part loop, after the engine line:

```cpp
            inst.sampler_speed_mode(p, smp[p].tapeIdx != 0);
            inst.sampler_reverse(p, smp[p].reverse);
            inst.sampler_feedback(p, smp[p].feedback);
```

- [ ] **Step 7: Build against the Rack SDK**

```bash
RACK_DIR=/path/to/Rack-SDK make -C host/vcv
```

Expected: compiles and links. If `SampleBuffer` is not visible, add
`#include "sampler/sample_buffer.h"` — `instrument.h` already pulls in
`sampler/sampler_engine.h`, which includes it, so this should not be needed.

If a real Rack SDK is not reachable, STOP and report BLOCKED rather than
committing untested — the build is this task's only gate.

- [ ] **Step 8: Commit**

```bash
git add host/vcv/src/Spotymod.cpp host/vcv/src/sampler_ui.hpp
git commit -m "feat(vcv): inject sampler memory and make ENG select the sampler

The record buffers are heap vectors, not by-value members like the echo
memory: 42 s of stereo per part is ~16 MB. reinit() now resizes them, and
restores each part's content length afterwards -- an M5a finding was that a
sample-rate change silently discarded a loaded sample. The resize moves to
onSampleRateChange so 32 MB are not reallocated on the audio thread.

ENG is remapped from Synth/Test tone to Synth/Sampler; the test tone survives
as a per-part dev flag the context menu will expose.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 5: REC — button, LED, monitoring

REC is a latching pad: on = record, off = stop with fade. Monitoring follows
REC automatically (`Instrument::sampler_record` already flips monitor —
verify at `engine/instrument.h:96-101` and do not duplicate it). REC on a
synth part is inert with a dark LED; ENG is the only mode selector.

The LED shows two things at once, which is what makes it worth having:
**recording → it pulses; idle with content → steady at the fill level; idle
and empty → dark.**

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp` (`configControls`, `pushParams`,
  `process`, `SpotymodWidget`'s light loop)

**Interfaces:**
- Consumes: `REC_A`/`REC_B`, `REC_A_L`/`REC_B_L` (Task 3);
  `Instrument::sampler_record(p, bool)`, `sampler_fill(p)`,
  `sampler_empty(p)` (`engine/instrument.h:96-104`); `SamplerPartState`
  (Task 4).

- [ ] **Step 1: Configure the REC latch**

In `configControls`, in the `WK_LATCH` case, before the ENGINE branch:

```cpp
                    if (c.id == REC_A || c.id == REC_B)
                        configSwitch(c.id, 0.f, 1.f, 0.f, "Record",
                                     {"Stopped", "Recording"});
                    else if (c.id == ENGINE_A || c.id == ENGINE_B)
```

(the existing ENGINE branch becomes the `else if`; the rest of the chain is
unchanged).

- [ ] **Step 2: Drive the engine from the latch**

In `pushParams`, inside the per-part loop, after the edit-layer pushes from
Task 4:

```cpp
            // REC is a latch, so its value IS the desired state -- an edge
            // trigger would miss a state restored from a saved patch. The
            // engine's set_recording is idempotent, and sampler_record flips
            // monitoring with it, so pushing every control tick is correct.
            // On a synth part REC is inert: ENG is the only mode selector.
            const bool wantRec = ppb(REC_A, p) && eng2 && !smp[p].testTone;
            if (wantRec != inst.sampler_is_recording(p)) inst.sampler_record(p, wantRec);
```

`Instrument` has no `sampler_is_recording` yet. Add it in `engine/instrument.h`
beside the other sampler queries:

```cpp
    bool sampler_is_recording(int p) const { return _parts[p].sampler().is_recording(); }
```

- [ ] **Step 3: Drive the LED**

In `process()`, replace the two gate-light lines (357-358) with:

```cpp
        lights[GATE_A_L].setBrightness(gateFilt[0]);
        lights[GATE_B_L].setBrightness(gateFilt[1]);

        // REC LED: pulsing while recording, steady at the fill level when the
        // part holds content, dark when empty or on a synth part. One LED, the
        // two things a player needs to know -- am I armed, and how full is it.
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            float b = 0.f;
            if (inst.sampler_is_recording(p)) {
                recPhase[p] += 2.f / args.sampleRate;      // 2 Hz pulse
                if (recPhase[p] >= 1.f) recPhase[p] -= 1.f;
                b = recPhase[p] < 0.5f ? 1.f : 0.25f;
            } else if (!inst.sampler_empty(p)) {
                b = 0.15f + 0.55f * inst.sampler_fill(p);
            }
            lights[p ? REC_B_L : REC_A_L].setBrightness(b);
        }
```

and add the phase member beside `gateFilt` (line 92):

```cpp
    float recPhase[2] = {0.f, 0.f};        // REC LED pulse while recording
```

- [ ] **Step 4: Give the REC LED its own colour**

In `SpotymodWidget`, replace the light loop (lines 503-504):

```cpp
        for (const auto& c : kLightCtls) {
            Vec pos = mm2px(Vec(c.mm.x, c.mm.y));
            if (c.id == REC_A_L || c.id == REC_B_L)   // record = red, the one
                addChild(createLightCentered<SmallLight<RedLight>>(pos, module, c.id));
            else                                       // gate glow = warm signal hue
                addChild(createLightCentered<MediumLight<YellowLight>>(pos, module, c.id));
        }
```

- [ ] **Step 5: Build and play-test**

```bash
RACK_DIR=/path/to/Rack-SDK make -C host/vcv && RACK_DIR=/path/to/Rack-SDK make -C host/vcv install
```

Then in Rack, by hand: patch a signal into IN L, flip ENG on part A to
Sampler, press REC — the LED pulses, and the part should sound *while*
recording (fill-follows). Release REC — the LED goes steady. Press REC on a
part whose ENG is on Synth — nothing happens, LED stays dark.

Report what was observed. If a real Rack is not available, report
DONE_WITH_CONCERNS naming exactly what could not be verified.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/src/Spotymod.cpp engine/instrument.h
git commit -m "feat(vcv): wire the REC pad, its LED and monitoring

REC is pushed as a state, not an edge, so a patch that saved with REC engaged
restores correctly. The LED carries both facts a player needs: pulsing while
recording, steady at the fill level when the part holds content, dark when
empty or when ENG is on Synth.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 6: The context menu edit layer

Everything that is not ENG or REC lives here, per the spec: *Load sample…*,
*Save sample…*, *Clear sample*, Tape/Digital, Reverse, overdub feedback, and
the *Engine: test tone* dev flag. Two submenus, one per part.

File dialogs use `osdialog`, which the Rack SDK bundles. Loading and saving
run on the UI thread — a 42 s WAV read is ~16 MB and takes long enough to
matter, but Rack menu actions are expected to be synchronous and the engine
keeps playing the old content until `load_sample` returns.

**Files:**
- Modify: `host/vcv/src/sampler_ui.hpp` (the load/save helpers)
- Modify: `host/vcv/src/Spotymod.cpp` (`appendContextMenu`, lines 520-527)
- Modify: `host/vcv/Makefile` (`-I$(REPO)/host` for the shared WAV headers)

**Interfaces:**
- Consumes: `spky::read_wav` / the writer (Task 2),
  `Instrument::load_sample`, `sampler_clear`, `sampler_data`,
  `sampler_rec_size` (Task 1), `SamplerPartState` (Task 4).
- Produces: `bool spkyvcv::load_wav_into(spky::Instrument&, int part, const std::string& path, float engine_sr, std::string& err)`,
  `bool spkyvcv::save_wav_from(const spky::Instrument&, int part, const std::string& path, float sr, std::string& err)`,
  and `void spkyvcv::resample_linear(std::vector<float>&, double ratio)`.

- [ ] **Step 1: Add the include path**

In `host/vcv/Makefile`, extend the FLAGS line (around line 15-22):

```make
FLAGS += -I$(REPO)/engine -I$(REPO)/third_party -I$(REPO)/host
```

- [ ] **Step 2: Add the load/save helpers**

Append to `host/vcv/src/sampler_ui.hpp`:

```cpp
#include <vector>
#include "instrument.h"
#include "shared/wav_reader.h"
#include "shared/wav_writer.h"

namespace spkyvcv {

// Linear resample to the engine's rate. A file recorded at 44.1 kHz would
// otherwise play ~9% sharp in a 48 kHz Rack -- that is an import bug, not the
// instrument's tape idiom (the tape idiom is what the PITCH knob and Tape
// mode do, deliberately, to material that is already in the buffer).
// Linear is enough: this runs once per load, off the audio thread, and the
// grain engine's own read is linear-interpolated too.
inline void resample_linear(std::vector<float>& v, double ratio) {
    if (v.empty()) return;
    const size_t n = (size_t)((double)v.size() * ratio);
    if (n < 2) return;
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) {
        const double src = (double)i / ratio;
        const size_t i0 = (size_t)src;
        const size_t i1 = i0 + 1 < v.size() ? i0 + 1 : i0;
        const float  f  = (float)(src - (double)i0);
        out[i] = v[i0] + (v[i1] - v[i0]) * f;
    }
    v.swap(out);
}

// Read a WAV off disk into a part's record buffer. The reader hands back
// deinterleaved channels, which is exactly what load_sample takes. A mono
// file arrives with l == r from the reader, so nothing special is needed.
// engine_sr is the rate the Instrument is currently running at.
inline bool load_wav_into(spky::Instrument& inst, int part,
                          const std::string& path, float engine_sr,
                          std::string& err) {
    spky::WavData d;
    if (!spky::read_wav(path, d, err)) return false;
    if (d.l.empty()) { err = "file contains no samples"; return false; }
    if (d.sample_rate > 0 && engine_sr > 0.f
        && (float)d.sample_rate != engine_sr) {
        const double ratio = (double)engine_sr / (double)d.sample_rate;
        resample_linear(d.l, ratio);
        resample_linear(d.r, ratio);
    }
    inst.load_sample(part, d.l.data(), d.r.data(), d.l.size());
    return true;
}

// Write a part's recorded content out. The frames belong to the host, and
// rec_size() is the valid length -- reading past it would emit whatever the
// buffer held before. WavWriter is a 16-bit PCM stereo writer that takes
// interleaved pushes, so there is no float vector to build.
inline bool save_wav_from(const spky::Instrument& inst, int part,
                          const std::string& path, float sr, std::string& err) {
    const size_t n = inst.sampler_rec_size(part);
    const spky::SampleBuffer::Frame* f = inst.sampler_data(part);
    if (!n || !f) { err = "nothing recorded"; return false; }
    spky::WavWriter w((int)sr);
    for (size_t i = 0; i < n; ++i) w.push(f[i].l, f[i].r);
    if (!w.write(path)) { err = "could not write " + path; return false; }
    return true;
}

}  // namespace spkyvcv
```

`WavWriter`'s exact API is in `host/shared/wav_writer.h`: constructor takes
the sample rate, `push(float l, float r)` per frame, `bool write(const std::string&)`.
Read it before writing and match it; adapt the call, never the writer.
`resample_linear` is host-side, off the audio thread — the `engine/` no-heap
rule does not apply to it.

- [ ] **Step 3: Build the menu**

Replace `appendContextMenu` (lines 520-527):

```cpp
    void appendContextMenu(Menu* menu) override {
        auto* m = getModule<Spotymod>();
        menu->addChild(new MenuSeparator);
        // Same gesture as a pulse into RST: zero the downbeat and restart the
        // loops at the bar start (a live STEPS turn leaves them free-running).
        menu->addChild(createMenuItem("Resync loops to bar", "",
                                      [m]() { m->resyncReq = true; }));

        menu->addChild(new MenuSeparator);
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            const std::string name = p ? "Sampler B" : "Sampler A";
            menu->addChild(createSubmenuItem(name, "", [m, p](Menu* sub) {
                sub->addChild(createMenuItem("Load sample...", "", [m, p]() {
                    char* path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, nullptr);
                    if (!path) return;
                    std::string err;
                    if (spkyvcv::load_wav_into(m->inst, p, path, m->curSr, err)) {
                        m->smp[p].path = path;
                        m->smp[p].factoryLoaded = false;
                    } else {
                        osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, err.c_str());
                    }
                    std::free(path);
                }));
                sub->addChild(createMenuItem("Save sample...", "", [m, p]() {
                    char* path = osdialog_file(OSDIALOG_SAVE, nullptr, "sample.wav", nullptr);
                    if (!path) return;
                    std::string err;
                    if (!spkyvcv::save_wav_from(m->inst, p, path, m->curSr, err))
                        osdialog_message(OSDIALOG_ERROR, OSDIALOG_OK, err.c_str());
                    std::free(path);
                }));
                sub->addChild(createMenuItem("Clear sample", "", [m, p]() {
                    m->inst.sampler_clear(p);
                    m->smp[p].path.clear();
                    m->smp[p].factoryLoaded = false;
                }));
                sub->addChild(new MenuSeparator);
                sub->addChild(createIndexPtrSubmenuItem(
                    "Speed mode", {"Digital", "Tape"}, &m->smp[p].tapeIdx));
                sub->addChild(createBoolPtrMenuItem("Reverse", "", &m->smp[p].reverse));
                sub->addChild(createSubmenuItem("Overdub feedback", "", [m, p](Menu* fb) {
                    fb->addChild(new FeedbackSlider(&m->smp[p].feedback));
                }));
                sub->addChild(new MenuSeparator);
                sub->addChild(createBoolPtrMenuItem("Engine: test tone (dev)", "",
                                                    &m->smp[p].testTone));
            }));
        }
    }
```

`createIndexPtrSubmenuItem` needs an `int`, not a `bool`, so add
`int tapeIdx = 1;` to `SamplerPartState` and make `tape` derived:
in `pushParams`, push `inst.sampler_speed_mode(p, smp[p].tapeIdx != 0);` and
drop the `tape` bool from the struct entirely rather than keeping two
representations of one fact.

Add the feedback quantity and slider above `SpotymodWidget`:

```cpp
// Overdub feedback is a continuous value with no panel home -- the menu
// slider is its only surface. 0.95 (~-3 dB) is the engine default. The knob
// is normalised 0..1; the engine maps it to -60..0 dB internally
// (SampleBuffer::set_feedback), so the display is a percentage of the knob,
// not a dB figure.
struct FeedbackQuantity : Quantity {
    float* v;
    explicit FeedbackQuantity(float* p) : v(p) {}
    void  setValue(float x) override { *v = clamp(x, 0.f, 1.f); }
    float getValue() override        { return *v; }
    float getMinValue() override     { return 0.f; }
    float getMaxValue() override     { return 1.f; }
    float getDefaultValue() override { return 0.95f; }
    std::string getLabel() override  { return "Overdub feedback"; }
    std::string getDisplayValueString() override {
        return string::f("%.0f%%", getValue() * 100.f);
    }
};

struct FeedbackSlider : ui::Slider {
    explicit FeedbackSlider(float* v) {
        box.size.x = 180.f;
        quantity = new FeedbackQuantity(v);
    }
    ~FeedbackSlider() override { delete quantity; }
};
```

Add `#include <osdialog.h>` and `#include <cstdlib>` at the top of
`Spotymod.cpp`.

- [ ] **Step 4: Build and play-test**

```bash
RACK_DIR=/path/to/Rack-SDK make -C host/vcv install
```

In Rack: load a WAV into part A, hear it granulate; save it back out and
re-load the saved file; clear it and confirm silence; flip Reverse and
Digital/Tape and hear both change; drag the feedback slider and confirm an
overdub over existing content builds instead of replacing.

- [ ] **Step 5: Commit**

```bash
git add host/vcv/src/Spotymod.cpp host/vcv/src/sampler_ui.hpp host/vcv/Makefile
git commit -m "feat(vcv): sampler edit layer in the context menu

Load/Save/Clear, Tape vs Digital, Reverse, overdub feedback and the test-tone
dev flag -- one submenu per part. The panel stays at ENG + REC, which is what
keeps it reducible to the hardware.

WAV I/O reuses host/shared's reader and writer rather than vendoring dr_wav:
they are already hardened against malformed chunk sizes and covered by
tests/test_wav.cpp.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 7: Persistence

The module has **no** `dataToJson`/`dataFromJson` today — all state rides on
Rack's automatic param save. Everything added in Tasks 4-6 is non-param
state and would be lost. So would `principleIdx[2]`, which is non-param state
that has *always* been lost on save (`Spotymod.cpp:90`); fix it here, since
this task is where the hooks appear.

Two levels, per the spec:
- **JSON:** the edit layer + the sample's path.
- **Patch storage:** the recorded audio itself, written as WAV into Rack's
  per-module patch-storage directory on save and re-read on load, so a
  live-recorded texture survives save/reopen even though it has no file.

**Files:**
- Modify: `host/vcv/src/Spotymod.cpp`

**Interfaces:**
- Consumes: everything from Tasks 4-6.
- Produces: `json_t* dataToJson() override`, `void dataFromJson(json_t*) override`,
  `void onSave(const SaveEvent&) override`, `void onAdd(const AddEvent&) override`.

- [ ] **Step 1: Verify the Rack patch-storage API before writing against it**

Rack v2 exposes per-module storage, but the exact names must be confirmed
against the SDK in hand rather than assumed. Grep the SDK:

```bash
grep -rn "PatchStorageDirectory" /path/to/Rack-SDK/include/
```

Expected: `Module::getPatchStorageDirectory()` and
`Module::createPatchStorageDirectory()` in `include/engine/Module.hpp`.
If they are absent in this SDK version, implement the JSON half only and
report the omission — do **not** invent a substitute path under the user's
home directory.

- [ ] **Step 2: Write the JSON hooks**

Add to `Spotymod`:

```cpp
    json_t* dataToJson() override {
        json_t* root = json_object();
        // Non-param state that has never been saved: the phrase principle is
        // cycled by a momentary button, so nothing in the param set records it.
        json_t* pr = json_array();
        for (int p = 0; p < spky::PART_COUNT; ++p)
            json_array_append_new(pr, json_integer(principleIdx[p]));
        json_object_set_new(root, "principle", pr);

        json_t* parts = json_array();
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            json_t* o = json_object();
            json_object_set_new(o, "path", json_string(smp[p].path.c_str()));
            json_object_set_new(o, "tape", json_integer(smp[p].tapeIdx));
            json_object_set_new(o, "reverse", json_boolean(smp[p].reverse));
            json_object_set_new(o, "feedback", json_real(smp[p].feedback));
            json_object_set_new(o, "testTone", json_boolean(smp[p].testTone));
            json_object_set_new(o, "factory", json_boolean(smp[p].factoryLoaded));
            json_array_append_new(parts, o);
        }
        json_object_set_new(root, "sampler", parts);
        return root;
    }

    void dataFromJson(json_t* root) override {
        if (!root) return;
        if (json_t* pr = json_object_get(root, "principle")) {
            for (int p = 0; p < spky::PART_COUNT; ++p) {
                json_t* v = json_array_get(pr, p);
                if (v) {
                    principleIdx[p] = (int)json_integer_value(v);
                    inst.set_principle(p, principleIdx[p]);
                }
            }
        }
        json_t* parts = json_object_get(root, "sampler");
        if (!parts) return;
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            json_t* o = json_array_get(parts, p);
            if (!o) continue;
            if (json_t* v = json_object_get(o, "path"))
                smp[p].path = json_string_value(v) ? json_string_value(v) : "";
            if (json_t* v = json_object_get(o, "tape"))     smp[p].tapeIdx = (int)json_integer_value(v);
            if (json_t* v = json_object_get(o, "reverse"))  smp[p].reverse = json_boolean_value(v);
            if (json_t* v = json_object_get(o, "feedback")) smp[p].feedback = (float)json_real_value(v);
            if (json_t* v = json_object_get(o, "testTone")) smp[p].testTone = json_boolean_value(v);
            if (json_t* v = json_object_get(o, "factory"))  smp[p].factoryLoaded = json_boolean_value(v);
        }
        // Content is restored in onAdd(): dataFromJson runs before the module
        // is in the engine, and the patch-storage directory is only meaningful
        // once it is.
        pendingRestore = true;
    }
```

with `bool pendingRestore = false;` added beside the other members.

- [ ] **Step 3: Write the audio to patch storage on save**

```cpp
    std::string storedWavPath(int p) {
        return system::join(createPatchStorageDirectory(),
                            p ? "sample_b.wav" : "sample_a.wav");
    }

    void onSave(const SaveEvent& e) override {
        Module::onSave(e);
        // The recorded texture has no file of its own -- without this it dies
        // with the session. Only content that did not come from a file or the
        // factory WAV needs writing: those two reload from their source.
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            if (!smp[p].path.empty() || smp[p].factoryLoaded) continue;
            if (!inst.sampler_rec_size(p)) continue;
            std::string err;
            if (!spkyvcv::save_wav_from(inst, p, storedWavPath(p), curSr, err))
                WARN("Spotymod: could not store sampler %d: %s", p, err.c_str());
        }
    }

    void onAdd(const AddEvent& e) override {
        Module::onAdd(e);
        if (!pendingRestore) return;
        pendingRestore = false;
        for (int p = 0; p < spky::PART_COUNT; ++p) {
            std::string err;
            if (!smp[p].path.empty()) {
                if (!spkyvcv::load_wav_into(inst, p, smp[p].path, curSr, err))
                    WARN("Spotymod: sampler %d could not reload %s: %s",
                         p, smp[p].path.c_str(), err.c_str());
                continue;
            }
            const std::string stored = system::join(getPatchStorageDirectory(),
                                                    p ? "sample_b.wav" : "sample_a.wav");
            if (system::isFile(stored))
                spkyvcv::load_wav_into(inst, p, stored, curSr, err);
        }
    }
```

Note the asymmetry and keep it: `createPatchStorageDirectory()` on save (it
creates the directory), `getPatchStorageDirectory()` on load (it must not).
Confirm both names against the SDK in Step 1.

- [ ] **Step 4: Build and play-test**

Record a texture into part A without loading any file, save the patch, close
it, reopen it — the texture must be there and sound. Then load a WAV into
part B, save/reopen — it must reload from its path (and `sample_b.wav` must
NOT have been written). Flip Reverse and Tape/Digital, save/reopen — both
must persist. Cycle PRIN a few times, save/reopen — the principle must stick,
which it never did before.

- [ ] **Step 5: Commit**

```bash
git add host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): persist the sampler state and the recorded audio

The module had no dataToJson at all, so every non-param value died with the
session -- including principleIdx, which the PRIN button cycles and nothing
recorded. The edit layer and the sample path now go into JSON; a live-recorded
texture, which has no file to point at, is written into Rack's patch storage
on save and reloaded on open.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 8: Factory sample and autoload

*First-user experience:* flipping ENG to Sampler on an empty part loads a
factory WAV and it sounds immediately — one pad press, nothing to configure.
The sample is a bounced spotymod drone: harmonically rich sustained material
granulates well and is on-brand.

It must never overwrite recorded, loaded or patch-persisted content
(`factoryLoaded` from Task 4 marks it, so REC/load/clear treat it like any
other content).

**Files:**
- Copy: `host/render/scenarios/assets/Multi_Bulti_FX_Seq_110_01.wav`
  → `host/vcv/res/factory.wav`
- Modify: `host/vcv/src/Spotymod.cpp` (`pushParams`)

**Interfaces:**
- Consumes: `asset::plugin(pluginInstance, "res/factory.wav")`,
  `spkyvcv::load_wav_into` (Task 6), `SamplerPartState::factoryLoaded` (Task 4).

- [ ] **Step 1: Copy in the factory sample**

Bastian supplied the file (2026-07-21). No rendering step — use it as it is.

**Licensing: settled.** The name looks like a commercial sample-pack file;
it is not. Bastian made it himself, so it ships with the plugin under the
project's own licence. Do not re-open this.

```bash
cp host/render/scenarios/assets/Multi_Bulti_FX_Seq_110_01.wav host/vcv/res/factory.wav
```

Verified properties: RIFF/WAVE, IEEE float32, **stereo, 44100 Hz**, 3 080 820
bytes, ~8.7 s. Note `host/render/scenarios/assets/` is git-ignored, but
`host/vcv/res/` is not — the copy must be committed, and it is ~3 MB of
binary that will live in the repo permanently.

It is 44.1 kHz while Rack commonly runs at 48 kHz. That is handled: Task 6's
`load_wav_into` resamples to the engine rate on load, so this file arrives at
the right pitch whatever Rack is running at. Do not pre-convert it.

- [ ] **Step 2: Check it before shipping it**

Listen to it, or at minimum confirm it decodes and does not clip
(`./build/spky_render` is not involved here; a quick read through
`spky::read_wav` in a scratch program, or any audio player, is enough).
This is the first sound a new user hears.

- [ ] **Step 3: Autoload on the engine flip**

In `pushParams`, in the per-part loop, right after the engine selection from
Task 4:

```cpp
            // First-user experience: flipping ENG to Sampler on an empty part
            // loads the factory drone, so one pad press makes sound. It never
            // overwrites content -- is_empty() is the whole guard, and once
            // loaded it behaves like any other sample (REC overdubs it, Clear
            // clears it, and factoryLoaded keeps it out of patch storage).
            if (eng2 && !smp[p].testTone && inst.sampler_empty(p)
                     && !factoryTried[p]) {
                factoryTried[p] = true;
                std::string err;
                const std::string fp = asset::plugin(pluginInstance, "res/factory.wav");
                if (spkyvcv::load_wav_into(inst, p, fp, err))
                    smp[p].factoryLoaded = true;
                else
                    WARN("Spotymod: factory sample unavailable: %s", err.c_str());
            }
```

with `bool factoryTried[2] = {false, false};` beside the other members.

`factoryTried` exists so a failed load or a deliberate *Clear sample* does not
re-trigger the autoload on the very next control tick — clearing a part must
leave it cleared. Reset it to `false` only in `onReset()`.

**Note this runs on the audio thread** (`pushParams` is called from
`process`). A ~1 MB WAV read there is a xrun. It is a one-time cost on a
deliberate user gesture, and the alternative — a UI-thread worker — is
disproportionate. Load the factory WAV **once into a member** at first use
and reuse it for the second part rather than reading the file twice:

```cpp
    spky::WavData factoryWav;      // loaded lazily, shared by both parts
    bool factoryWavTried = false;
```

Implement the autoload against that cached copy: read the file at most once
per module instance, then `inst.load_sample(p, factoryWav.l.data(), factoryWav.r.data(), factoryWav.l.size())`.

- [ ] **Step 4: Build and play-test**

Fresh Spotymod instance → flip ENG on part A → sound within one gesture,
without touching anything else. Then: record over it (REC must overdub or
replace per the engine's rules, not be blocked); *Clear sample* → silence and
it must stay silent, not reload; save/reopen a patch whose part is on the
factory sample → it reloads from `res/factory.wav`, and no
`sample_a.wav` was written to patch storage.

- [ ] **Step 5: Commit**

```bash
git add host/vcv/res/factory.wav host/vcv/src/Spotymod.cpp
git commit -m "feat(vcv): factory sample autoloads on the first ENG flip

Flipping a part to Sampler with an empty buffer loads a factory WAV, so the
deck sounds within one gesture instead of demanding a recording first. It
never overwrites content, and once loaded it behaves like any other sample.

The file is 44.1 kHz; load_wav_into resamples it to whatever rate Rack is
running at, so it arrives in tune either way.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Task 9: Build verification, neutrality gate, documentation

The engine's two gates must still hold after M5b, and the plugin must build
clean from a cold tree.

**Files:**
- Modify: `host/vcv/README.md`
- Modify: `docs/milestone-history.md` (append the M5b entry)

- [ ] **Step 1: Run the full engine suite**

```bash
cmake --build build --target spky_tests && ./build/spky_tests -ts="*"
```

Expected: all green (481 before Task 1, plus Task 1's two new cases).

- [ ] **Step 2: Run the synth-neutrality gate**

Render the pinned baseline scenarios and compare byte-for-byte against the
pre-M5b commit. **`CMAKE_BUILD_TYPE` must be identical between the two runs**
— a Debug-vs-Release mismatch reports all scenarios as moved and is a false
alarm (this cost a full investigation during M5a). Follow the procedure
recorded in `docs/milestone-history.md`.

Expected: byte-identical. M5b touches no engine audio path except Task 1's
two accessors, so any difference here is a real defect.

- [ ] **Step 3: Cold build of the plugin**

```bash
rm -rf host/vcv/build host/vcv/dist
RACK_DIR=/path/to/Rack-SDK make -C host/vcv
RACK_DIR=/path/to/Rack-SDK make -C host/vcv dist
```

Expected: builds and produces a distributable with `res/factory.wav`
included (`DISTRIBUTABLES += res plugin.json` already covers `res/`).
Confirm `plugin.json` still reads `2.7.0` and was not touched.

- [ ] **Step 4: Update the README**

In `host/vcv/README.md`, document: ENG now selects Synth ↔ Sampler; REC
records from IN L/R with the LED showing record/fill; the per-part sampler
submenu; that the factory sample autoloads on an empty part; and that a
recorded texture survives patch save/reopen. Match the file's existing tone
and structure — read it before writing.

- [ ] **Step 5: Append the M5b milestone entry**

In `docs/milestone-history.md`, append an M5b section in the established
format: what shipped, the commit range, the two spec deviations and why, and
anything found during implementation that a later session would not
re-derive from the code.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/README.md docs/milestone-history.md
git commit -m "docs(m5b): document the sampler's VCV surface

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Verification summary

| Gate | Covers | Task |
|------|--------|------|
| doctest suite | the engine additions | 1, 9 |
| `test_panel.py` | param/light id stability, glyph overlap, widget kinds | 3 |
| Rack SDK build | every host change compiles and links | 4, 5, 6, 9 |
| synth neutrality (byte-identical) | no engine regression | 9 |
| determinism (byte-identical) | no engine regression | 9 |
| play-test by ear | everything the host does — this is the real gate | 5, 6, 7, 8 |

**Honest limitation:** the VCV host has no unit tests and this plan does not
add a harness for one — Rack's `Module` cannot be instantiated outside the
SDK's runtime. Tasks 4-8 are verified by compilation plus the human's
play-test. Any implementer who cannot reach a real Rack SDK must report
that rather than marking a task done on a compile alone.
