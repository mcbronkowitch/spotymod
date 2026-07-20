# M5a — The sampler texture deck (engine + render host) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A granular cloud as a third part engine — chord-locked, scale-quantized, fed from live input or a loaded WAV — verifiable end-to-end by doctest and by a scenario render, with the synth path proven bit-identical.

**Architecture:** A new `engine/sampler/` holds four files: a hardened port of the original firmware's `Buffer` record state machine, a latched-at-spawn `Grain`, tuning constants, and a `SamplerEngine` implementing `IPartEngine`. `Part` gains a `SamplerEngine` member alongside `_synth`, threads per-sample input into the active engine via a new `process_in` virtual, and forwards its composed gate on edges via a new `set_gate` virtual. `Instrument` finally routes its currently-discarded `inL`/`inR` pointers through to the parts. The desktop render host gains a WAV reader, an `input_wav` scenario field and a `load_wav` action, so live-recording scenarios render end-to-end.

**Tech Stack:** C++17, doctest, CMake + Ninja (desktop tests), DaisySP (vendored, `daisysp::Svf` only), nlohmann/json (vendored, render host only).

**Scope:** This plan is **M5a**. The VCV host — ENG remap, REC pad, `gen_panel.py`, context menu, `dr_wav`, `dataToJson`, factory sample — is **M5b**, planned separately once this engine is built and the `Instrument` API has proven stable. Nothing in this plan touches `host/vcv/`.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md` (amended 2026-07-20). It is the authority; this plan implements its engine and render-host half.
- Branch: `sampler-deck`. **Never bump `plugin.json` (stays 2.7.0). Never create a `v*` tag.**
- Commit trailer, every commit: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- **Never `git add -A`.** Stage explicit paths only.
- **Hard gate — synth neutrality.** No existing PITCH, groove, CHOKE, chord, FILT or Center test may be edited, and no baseline scenario render may change by a byte. Task 6 makes this an executable gate. If a melodic-path test needs touching, **stop and report** — the design is violated.
- `NDEBUG` is defined nowhere; `assert()` is live in the audio path. Do not add asserts to per-sample code.
- No allocation, no `std::` math beyond what the file already includes, in any per-sample path. `std::pow` is permitted in control-rate setters only (precedent: `SynthEngine::set_filt`'s cutoff mapping, `synth_engine.cpp:16-19`).
- **No heap in `engine/`.** The sample buffer is host-injected through `FxMem`, exactly as the echo buffers are. `nullptr` must degrade to a silent part, never a crash.
- **Determinism:** one `Rng` per engine, seeded `seed_base ^ <constant>` in `init`, never from time. No `std::` random machinery. A double render must be bit-identical.
- Every new test must be **mutation-tested**: break the implementation deliberately, confirm the test fails, restore. A test that passes against a broken implementation is not a test. Report the mutation and its result in the task's completion notes.
- **Observability requirement — this project's dominant defect class.** The taps plan produced three tests that passed against the very mutation they existed to catch; every one had a correct *assertion* but a *setup* that put the sample point where the property could not be seen. So for every test here, before running it, answer explicitly: **at the moment this assertion runs, what value would the broken code produce, and is it different from the correct one?** If the answer is "the same", change the *configuration*, not the assertion. Statistical tests (scatter, SUB share, DTUN spread) are the acute risk in this plan: a spread assertion with too few samples, or drawn from a seed whose first draws happen to cluster, passes against a hardcoded constant. Assert the *precondition* (e.g. `REQUIRE(spawns >= 200);`) alongside the property.
- Build and test:
  ```bash
  source env.sh
  cmake -S . -B build -G Ninja
  cmake --build build
  ./build/spky_tests
  ```
  Single case: `./build/spky_tests -tc="<exact test case name>"`

### Deliberate deviations from the spec

Three, all recorded here so a reviewer does not read them as drift. Each is implemented in the task named.

1. **Voice-row forwarding stays in `Part` (Task 5).** The spec asks for six new `Instrument` setters (`sampler_window`, `sampler_filt`, …) plus a VCV layer that pushes the voice row to both engines every frame. The existing code already solves this: `Part::set_voice_*` forwards straight to `_synth` so edits stick while another engine is active (`part.h:78-83`). Those six setters forward to `_sampler` as well. Same behaviour, no new `Instrument` API, **zero host changes** — where the literal reading costs 18 lines across three layers.
2. **`set_gate` is forwarded on edges, not per sample (Task 6).** The spec says `Part` forwards its composed gate signal. `Part::gate()` changes sample-accurately, but it is a bool with a handful of transitions per second; a virtual call per sample for it is pure cost. Forwarded as `if (g != _last_gate) { _engine->set_gate(g); _last_gate = g; }`, which is the established idiom for `set_cycle` ("forward on change, not per sample", `part.cpp:169-174`). The engine swap re-pushes the current value, as it already does for `set_flow`/`set_hold`.
3. **Monitoring is switched by `Instrument::sampler_record`, not by each host (Task 6).** The spec assigns "host enables it automatically while REC is on" to the host. Putting it in `Instrument` gives both hosts the behaviour for free and removes a way to get it wrong; `SamplerEngine::set_monitor` remains public so a host can still override.

### Two spec items that are already done

Do not re-implement these; verify and move on.

- **IN L/R are already wired in the VCV host**, including the L→R normal and ±5 V→±1 scaling (`host/vcv/src/Spotymod.cpp:337-341`). The spec lists this as M5 work. Only the *engine* discards the samples — that is Task 6.
- **`Instrument::process` already carries the input pointers** in its signature; they are commented out at the definition (`engine/instrument.cpp:66-67`). Threading them is a two-line change, not a refactor.

### One hazard the spec does not mention

`SampleBuffer::read_linear` is called by up to 8 grains per sample. The original `Buffer::_read` writes `_read_head` on every read (`src/core/buffer.cpp:115-120`), making it a non-const method that mutates shared state from eight independent readers. The port **drops `_read_head` entirely** — grains own their position — which makes the read `const` and reentrant. This is not an optimisation; the original's design is simply wrong for a multi-grain reader.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `engine/sampler/sampler_config.h` | **New.** Tuning constants: fade length, feedback default, grain count, size curve, scatter ranges, FILT rails. | 1 |
| `engine/sampler/sample_buffer.h` / `.cpp` | **New.** Hardened port of `src/core/buffer.*`: record fade state machine, overdub feedback write, cut, fill/empty queries, clear, const interpolated read. | 1 |
| `engine/sampler/grain.h` | **New.** One grain: latched start/ratio/pan/window/direction, Hann attack+decay halves, interpolated stereo read. Header-only. | 2 |
| `engine/sampler/sampler_engine.h` / `.cpp` | **New.** `IPartEngine`: scheduler, chord round-robin, scatter, gating, filter, transport, record/monitor/load. | 3, 4, 5 |
| `engine/parts/engine_iface.h` | **Modify.** `ENGINE_SAMPLER = 2`; `process_in` and `set_gate` virtuals with no-op defaults. | 6 |
| `engine/parts/part.h` / `.cpp` | **Modify.** `SamplerEngine` member, `_engine_for` switch, input threading, gate edge forwarding, voice-row dual forward, sampler memory in `init`. | 5, 6 |
| `engine/instrument.h` / `.cpp` | **Modify.** `FxMem` gains the sampler buffers; `process` threads `inL`/`inR`; sampler API surface. | 6 |
| `host/render/wav_reader.h` | **New.** Chunk-walking RIFF reader (16/24/32-bit PCM + float, mono→stereo). | 7 |
| `host/render/scenario.h` / `.cpp` | **Modify.** `input_wav` top-level field; `load_wav`, `sampler_record`, `sampler_clear` and edit-layer actions. | 7 |
| `host/render/main.cpp` | **Modify.** Feed `input_wav` samples into `process`; two new CSV columns per part. | 7 |
| `tests/test_sample_buffer.cpp` | **New.** Fades, overdub level, wrap, empty/nullptr hardening. | 1 |
| `tests/test_grain.cpp` | **New.** Window shape, pan law, pitch ratio, reverse, lifetime. | 2 |
| `tests/test_sampler_engine.cpp` | **New.** FLOW continuity, STEP gating, chord distribution, scatter statistics, tape/digital, voice row, determinism. | 3, 4, 5 |
| `tests/test_sampler_part.cpp` | **New.** Engine switch, input threading, gate forwarding, CHOKE hold, synth neutrality. | 6 |
| `CMakeLists.txt` | **Modify.** New engine sources + test files. | 1, 2, 3, 6, 7 |
| `host/vcv/Makefile` | **Modify.** New engine sources only (the two lists are meant to stay identical — `Makefile:32`). | 6 |

**Not touched by this plan:** `host/vcv/src/*`, `res/gen_panel.py`, `res/test_panel.py`, `plugin.json`, `src/*` (the frozen reference).

---

### Task 1: `SampleBuffer` — the record core, hardened

**Files:**
- Create: `engine/sampler/sampler_config.h`
- Create: `engine/sampler/sample_buffer.h`, `engine/sampler/sample_buffer.cpp`
- Test: `tests/test_sample_buffer.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `spky::SoftSwitch` and `spky::hann_value_at` from `engine/fx/fx_util.h`; `spky::clampf`, `spky::lerpf` from `engine/util/math.h`.
- Produces:
  - `struct SampleBuffer::Frame { float l; float r; };`
  - `void init(Frame* buf, size_t length)` — pointer injection, no ownership
  - `void read_linear(float frame, float& out0, float& out1) const`
  - `void set_recording(bool on)`, `bool is_recording() const`, `bool is_overdubbing() const`
  - `void set_feedback(float knob)` (0..1 → −60..0 dB), `void write(float in0, float in1)`
  - `void cut()`, `void clear()`, `void set_rec_size(size_t)`
  - `float fill() const`, `size_t rec_size() const`, `bool is_empty() const`, `bool valid() const`, `size_t capacity() const`
  - namespace `spky::sampler_cfg` with the constants listed in Step 1.

**Background — what is being ported.** `src/core/buffer.*` is the original firmware's record buffer, proven on hardware. Its state machine is `idle → fadein → sustain → fadeout`, with a 192-sample (4 ms) Hann fade at each end, and an overdub path where the existing content is attenuated by a feedback factor as new material is written over it. Five things change in the port, each for a stated reason:

1. `_read_head` is dropped (see "One hazard" above).
2. `#include "xfade.h"` is dropped — `buffer.cpp` never uses `XFade`.
3. `daisysp::pow10f` → `std::pow(10.f, x)`; `infrasonic::map(sw, 0,1, 1,_feedback)` → `lerpf(1.f, _feedback, sw)` (identical arithmetic, and `lerpf` is already in `engine/util/math.h`).
4. `_target_length` and `_wrap_counter` are dropped — declared in the original, never referenced.
5. **Empty-buffer hardening.** The original's `_read` does `frame %= _size` (division by zero when empty) and `read_linear` does `while (frame < 0) frame += _size` (infinite loop when empty). Both are unreachable in the original's slice-player usage and both are reachable here, because the cloud runs while the buffer is still filling. The port guards `_size == 0` before either.
6. **Punch-in position.** The original starts an overdub at the read head (`_write_head = _read_head - min(1, _read_head)`). A cloud has no single playhead, so an overdub starts at frame 0. Simpler, and the only defensible choice without a playhead.

- [ ] **Step 1: Write the constants header**

Create `engine/sampler/sampler_config.h`:

```cpp
#pragma once
#include <cstddef>

// Tuning constants for the M5 texture deck (spec 2026-07-18
// sampler-texture-deck-design.md). Values marked "ear-tunable" are taste,
// not contract -- changing them changes the sound, not the correctness.
namespace spky {
namespace sampler_cfg {

// --- record core (carried over from src/core/config.h:57,73) ---
// 192 samples == 4 ms @ 48 kHz, and == the hann table size in fx_util.h, so
// the fade counter indexes the curve 1:1. Both facts are load-bearing.
constexpr size_t kRecordFade      = 192;
constexpr float  kDefaultFeedback = 0.95f;   // knob position; -3 dB on the -60..0 dB curve

// --- the cloud ---
constexpr int    kGrains        = 8;         // per part
constexpr int    kCtrlInterval  = 96;        // must equal SynthEngine::kCtrlInterval

// SIZE: exponential 20 ms .. 2 s, size_s = kSizeMinS * kSizeRange^n
constexpr float  kSizeMinS      = 0.02f;
constexpr float  kSizeRange     = 100.f;

// Grain window: the ATK/DEC halves each span at most this fraction of the
// grain, so a fully-open ATK and DEC still leave the window a real shape
// rather than two ramps meeting at a point.
constexpr float  kWindowHalfMax = 0.5f;
// ...and at least this fraction, so a closed knob is still click-free.
constexpr float  kWindowHalfMin = 0.02f;

// STEP burst: grains keep spawning this long past the gate falling, so a
// chopped texture ends with a tail rather than a cut. Ear-tunable.
constexpr float  kBurstReleaseS = 0.06f;

// MOTION scatter, at MOTION = 1 (all ear-tunable):
constexpr float  kScatterPosFrac  = 0.25f;   // +-1/4 of content length
constexpr float  kScatterTimeFrac = 0.75f;   // spawn-interval jitter, fraction of interval
constexpr float  kScatterOctProb  = 0.25f;   // chance a chord note jumps an octave

// --- voice row, remapped ---
constexpr float  kCutoffMinHz   = 60.f;      // same rails as the synth FILTER
constexpr float  kCutoffMaxHz   = 14000.f;
// The FILT fade invariant, mirrored from SynthEngine (synth_engine.h:44-48):
// the left half overdrives the rail by exactly the blend zone, so t = -1
// lands in silence at ANY lane position. Invariant: kFiltLeftScale >= 1 + kFiltFadeRange.
constexpr float  kFiltLeftScale = 1.25f;
constexpr float  kFiltFadeRange = 0.25f;
constexpr float  kDetuneCeilCt  = 35.f;      // DTUN spread ceiling, matches the synth
constexpr float  kSubMaxShare   = 1.f;       // SUB 1 = every grain an octave down

}  // namespace sampler_cfg
}  // namespace spky
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_sample_buffer.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kCap = 4800;   // 100 ms @ 48k -- plenty, and fast

// A buffer with its storage. Kept in one place so no test forgets init().
struct Fixture {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    Fixture() { buf.init(mem.data(), kCap); }
};

// Record `n` samples of a constant value; returns the peak |delta| between
// consecutive written frames, which is what a missing fade shows up as.
static float record_const(SampleBuffer& b, float v, size_t n) {
    b.set_recording(true);
    for (size_t i = 0; i < n; ++i) b.write(v, v);
    b.set_recording(false);
    // drain the fade-out: it needs kRecordFade more write() calls to finish
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) b.write(v, v);
    return 0.f;
}

TEST_CASE("sample_buffer: record fades in and out with no discontinuity") {
    Fixture f;
    record_const(f.buf, 1.f, 2400);
    REQUIRE(f.buf.rec_size() > sampler_cfg::kRecordFade * 2);

    // Walk the recorded content: no step between neighbours may exceed the
    // fade's own per-sample slope. A missing fade produces a step of ~1.0.
    float worst = 0.f;
    for (size_t i = 1; i < f.buf.rec_size(); ++i) {
        const float d = std::fabs(f.mem[i].l - f.mem[i - 1].l);
        if (d > worst) worst = d;
    }
    // The steepest point of a 192-sample Hann rise is ~pi/(2*192) ~= 0.0082.
    CHECK(worst < 0.02f);
    // ...and the fade really happened: the first frame is near silence.
    CHECK(f.mem[0].l < 0.05f);
    // ...while the middle is at full level (else "no discontinuity" is
    // trivially satisfied by recording nothing at all).
    CHECK(f.mem[1200].l > 0.95f);
}

TEST_CASE("sample_buffer: overdub attenuates the old content by the feedback") {
    Fixture f;
    // Pass 1: fill with 1.0, then lock the loop length.
    f.buf.set_recording(true);
    for (size_t i = 0; i < 2400; ++i) f.buf.write(1.f, 1.f);
    f.buf.set_recording(false);
    for (size_t i = 0; i < sampler_cfg::kRecordFade + 2; ++i) f.buf.write(1.f, 1.f);
    f.buf.cut();
    const size_t len = f.buf.rec_size();
    REQUIRE(len > 0);

    // Pass 2: overdub silence at feedback 0.5. The old content must be
    // scaled by ~0.5 in the sustain region, not left alone and not erased.
    f.buf.set_feedback(0.5f);          // knob 0.5 -> -30 dB, NOT a 0.5 factor
    const float fb = std::pow(10.f, (60.f * (0.5f - 1.f)) * 0.05f);
    f.buf.set_recording(true);
    for (size_t i = 0; i < len; ++i) f.buf.write(0.f, 0.f);
    f.buf.set_recording(false);

    // Sample well inside the loop, past the fade-in and before the fade-out.
    const size_t probe = len / 2;
    CHECK(f.mem[probe].l == doctest::Approx(fb).epsilon(0.05));
    // Guard the precondition: if the loop were shorter than the two fades,
    // `probe` would sit inside a fade and this test would prove nothing.
    REQUIRE(len > sampler_cfg::kRecordFade * 4);
}

TEST_CASE("sample_buffer: empty buffer reads silence and never hangs") {
    Fixture f;
    REQUIRE(f.buf.is_empty());
    float l = 1.f, r = 1.f;
    f.buf.read_linear(0.f, l, r);        // the original divides by zero here
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    f.buf.read_linear(-5.f, l, r);       // the original loops forever here
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("sample_buffer: nullptr memory is inert, not a crash") {
    SampleBuffer b;                       // never init()ed
    CHECK_FALSE(b.valid());
    b.set_recording(true);
    b.write(1.f, 1.f);                    // must not dereference
    float l = 1.f, r = 1.f;
    b.read_linear(3.5f, l, r);
    CHECK(l == 0.f);
    CHECK(r == 0.f);
    CHECK(b.is_empty());
}

TEST_CASE("sample_buffer: read_linear interpolates and wraps at the content length") {
    Fixture f;
    // Hand-place a 4-frame ramp and declare it the content.
    for (size_t i = 0; i < 4; ++i) { f.mem[i].l = float(i); f.mem[i].r = -float(i); }
    f.buf.set_rec_size(4);
    REQUIRE(f.buf.rec_size() == 4);

    float l = 0.f, r = 0.f;
    f.buf.read_linear(1.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
    CHECK(r == doctest::Approx(-1.5f));

    // Frame 3 -> frame 0 is the wrap seam: 3 + 0.5*(0 - 3) = 1.5
    f.buf.read_linear(3.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));

    // Past the end and before the start both fold back into range.
    f.buf.read_linear(5.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
    f.buf.read_linear(-2.5f, l, r);
    CHECK(l == doctest::Approx(1.5f));
}

TEST_CASE("sample_buffer: recording past capacity auto-stops with the loop locked") {
    Fixture f;
    f.buf.set_recording(true);
    for (size_t i = 0; i < kCap + 500; ++i) f.buf.write(0.5f, 0.5f);
    CHECK(f.buf.rec_size() == kCap);
    CHECK(f.buf.fill() == doctest::Approx(1.f));
    CHECK(f.buf.is_overdubbing());   // the loop is locked; further writes overdub
}

TEST_CASE("sample_buffer: clear returns it to empty") {
    Fixture f;
    record_const(f.buf, 1.f, 2400);
    REQUIRE_FALSE(f.buf.is_empty());
    f.buf.clear();
    CHECK(f.buf.is_empty());
    CHECK(f.buf.fill() == doctest::Approx(0.f));
    CHECK_FALSE(f.buf.is_recording());
    CHECK(f.mem[1200].l == 0.f);
}
```

- [ ] **Step 3: Run the test to verify it fails**

Add the test to `CMakeLists.txt` first — locate the `add_executable(spky_tests` block and add both lines next to their siblings (the list interleaves engine sources with their tests):

```cmake
    engine/sampler/sample_buffer.cpp
    tests/test_sample_buffer.cpp
```

Run:
```bash
source env.sh && cmake -S . -B build -G Ninja && cmake --build build
```
Expected: **compile failure**, `fatal error: sampler/sample_buffer.h: No such file or directory`.

- [ ] **Step 4: Write the header**

Create `engine/sampler/sample_buffer.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "fx/fx_util.h"   // SoftSwitch, hann_value_at

namespace spky {

// The record buffer of the M5 texture deck: a port of the original firmware's
// src/core/buffer.* (proven on hardware) with the libDaisy-adjacent pieces
// stripped and six deliberate changes, all listed in the M5a plan, Task 1.
//
// Memory is INJECTED (engine "no heap" contract): the host owns the frames,
// this class owns only the state machine. An un-init'ed buffer is inert.
//
// The read is const and holds no cursor -- up to kGrains readers call
// read_linear() concurrently within one sample, so a shared read head (which
// the original has) would be shared mutable state between them.
class SampleBuffer {
public:
    struct Frame { float l; float r; };

    SampleBuffer() = default;
    SampleBuffer(const SampleBuffer&) = delete;
    SampleBuffer& operator=(const SampleBuffer&) = delete;

    // buf may be nullptr: the buffer then reports !valid() and every
    // operation is a no-op. Hosts that cannot spare the memory rely on this.
    void init(Frame* buf, size_t length);

    // Linear-interpolated stereo read at a fractional frame index. Folds
    // out-of-range positions back into [0, rec_size). Silent when empty.
    void read_linear(float frame, float& out0, float& out1) const;

    // --- transport ---
    void set_recording(bool on);
    bool is_recording() const { return _state != State::idle; }
    bool is_overdubbing() const { return _cut.is_on() && is_recording(); }
    void set_feedback(float knob);    // 0..1 knob -> -60..0 dB linear factor
    void write(float in0, float in1); // call once per sample, always
    void cut();                       // lock the loop length at the fill
    void clear();
    void set_rec_size(size_t frames); // load path + tests: declare content

    // --- queries ---
    float  fill() const { return _buffer_size ? float(_size) / float(_buffer_size) : 0.f; }
    size_t rec_size() const { return _size; }
    bool   is_empty() const { return _size == 0; }
    bool   valid() const { return _buffer != nullptr && _buffer_size > 0; }
    size_t capacity() const { return _buffer_size; }
    Frame* raw() const { return _buffer; }

private:
    enum class State : uint8_t { idle, fadein, sustain, fadeout };

    SoftSwitch _cut;
    Frame*  _buffer      = nullptr;
    size_t  _buffer_size = 0;
    float   _feedback    = 0.f;   // linear factor; set by set_feedback()
    size_t  _size        = 0;     // recorded content length, in frames
    size_t  _write_head  = 0;
    size_t  _fade_ctr    = 0;
    State   _state       = State::idle;
};

}  // namespace spky
```

- [ ] **Step 5: Write the implementation**

Create `engine/sampler/sample_buffer.cpp`:

```cpp
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
#include "util/math.h"
#include <cmath>
#include <cstring>

namespace spky {

namespace {
constexpr float kFadeKof = 1.f / static_cast<float>(sampler_cfg::kRecordFade);
}

void SampleBuffer::init(Frame* buf, size_t length) {
    _buffer      = buf;
    _buffer_size = buf ? length : 0;
    _feedback    = std::pow(10.f, (60.f * (sampler_cfg::kDefaultFeedback - 1.f)) * 0.05f);
    clear();
}

void SampleBuffer::set_feedback(float knob) {
    // 0..1 knob mapped onto -60..0 dB, then to a linear factor. Control rate
    // only -- std::pow must never reach the per-sample path.
    const float dbfs = 60.f * (clampf(knob, 0.f, 1.f) - 1.f);
    _feedback = std::pow(10.f, dbfs * 0.05f);
}

void SampleBuffer::set_recording(bool on) {
    if (!valid()) return;
    switch (_state) {
        case State::idle:
            if (on) {
                // No playhead in a cloud, so an overdub punches in at 0
                // rather than at a read cursor (plan Task 1, change 6).
                if (!_cut.is_on()) _write_head = _size;
                else               _write_head = 0;
                _state     = State::fadein;
                _fade_ctr  = 0;
            }
            break;
        case State::fadeout:
            break;                       // already stopping; ignore
        default:
            if (!on) _state = State::fadeout;
            break;
    }
}

void SampleBuffer::write(float in0, float in1) {
    if (!valid()) return;

    float fade = 1.f;
    switch (_state) {
        case State::idle:
            return;
        case State::sustain:
            break;
        case State::fadein:
            fade = hann_value_at(static_cast<float>(_fade_ctr) * kFadeKof);
            if (++_fade_ctr >= sampler_cfg::kRecordFade - 1) _state = State::sustain;
            break;
        case State::fadeout:
            fade = hann_value_at(static_cast<float>(_fade_ctr) * kFadeKof);
            if (_fade_ctr == 0) { cut(); _state = State::idle; return; }
            --_fade_ctr;
            break;
    }

    // Feedback only bites where the fade is open, so a fading edge never
    // scrubs content it is not yet writing to. (Original: buffer.cpp:142-143.)
    const float fb      = lerpf(1.f, _feedback, _cut.process());
    const float fb_fade = clampf(1.f - fade * (1.f - fb), 0.f, 1.f);

    Frame f = _buffer[_write_head];
    f.l = in0 * fade + f.l * fb_fade;
    f.r = in1 * fade + f.r * fb_fade;
    _buffer[_write_head] = f;

    ++_write_head;
    if (_cut.is_on()) {
        if (_write_head >= _size) _write_head = 0;        // locked loop
    } else if (_write_head >= _buffer_size) {             // capacity reached
        _size = _buffer_size;
        cut();
    } else {
        if (_write_head > _size) _size = _write_head;     // free-run growth
    }
}

void SampleBuffer::cut() {
    if (_cut.is_on()) return;
    _cut.set_on(true);
    _write_head = 0;
}

void SampleBuffer::set_rec_size(size_t frames) {
    if (!valid()) return;
    _size = frames > _buffer_size ? _buffer_size : frames;
    cut();
}

void SampleBuffer::clear() {
    if (_buffer && _buffer_size)
        std::memset(_buffer, 0, sizeof(Frame) * _buffer_size);
    _write_head = 0;
    _size       = 0;
    _fade_ctr   = 0;
    _state      = State::idle;
    _cut.set_on(false, true);
}

void SampleBuffer::read_linear(float frame, float& out0, float& out1) const {
    // Empty or un-init'ed: silence. The original divides by zero on the
    // first branch and spins forever on the second (plan Task 1, change 5).
    if (_size == 0 || _buffer == nullptr) { out0 = 0.f; out1 = 0.f; return; }

    const float fsz = static_cast<float>(_size);
    // Bounded: a grain advances by at most ~4 frames per sample (+24 st) and
    // is folded every sample, so these loops run at most a couple of times.
    while (frame >= fsz) frame -= fsz;
    while (frame < 0.f)  frame += fsz;

    size_t i0 = static_cast<size_t>(frame);
    if (i0 >= _size) i0 = 0;                     // float edge at fsz - epsilon
    size_t i1 = i0 + 1;
    if (i1 >= _size) i1 = 0;

    const float frac = frame - static_cast<float>(i0);
    const Frame a = _buffer[i0];
    const Frame b = _buffer[i1];
    out0 = a.l + frac * (b.l - a.l);
    out1 = a.r + frac * (b.r - a.r);
}

}  // namespace spky
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build && ./build/spky_tests -tc="sample_buffer:*"
```
Expected: all 7 cases PASS.

Then confirm nothing else moved:
```bash
./build/spky_tests
```
Expected: the full suite passes, same case count as before plus 7.

- [ ] **Step 7: Mutation-test every case**

Apply each mutation, rebuild, confirm the named test fails, then restore. Record the results in the completion notes.

| # | Mutation | Must break |
|---|---|---|
| 1 | In `write`, replace `fade = hann_value_at(...)` in the `fadein` branch with `fade = 1.f` | *record fades in and out* |
| 2 | In `write`, change `fb_fade` to a constant `1.f` | *overdub attenuates…* |
| 3 | In `read_linear`, delete the `_size == 0` guard | *empty buffer reads silence* (hangs or crashes — a hang counts as a break, kill it and note that) |
| 4 | In `read_linear`, replace `frac` with `0.f` | *interpolates and wraps* |
| 5 | In `write`, delete the `_write_head >= _buffer_size` branch | *recording past capacity* |
| 6 | In `clear`, delete the `memset` | *clear returns it to empty* |

If any mutation leaves its test green, the test is measuring the wrong thing — fix the **setup**, not the assertion, and re-run.

- [ ] **Step 8: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sample_buffer.h \
        engine/sampler/sample_buffer.cpp tests/test_sample_buffer.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
sampler: port the record buffer as SampleBuffer

Hardened port of src/core/buffer.*: the shared read head is gone (eight
grains read concurrently), the empty-buffer divide-by-zero and infinite
loop are guarded, and the dead xfade include, _target_length and
_wrap_counter are dropped. src/ stays untouched.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: `Grain` — one voice of the cloud

**Files:**
- Create: `engine/sampler/grain.h` (header-only, like `engine/fx/limiter.h`)
- Test: `tests/test_grain.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `SampleBuffer::read_linear` (Task 1); `spky::hann_value_at` (`engine/fx/fx_util.h`); `spky::fast_sin` (`engine/util/fast_sin.h`); `spky::clampf` (`engine/util/math.h`).
- Produces:
  - `void Grain::spawn(float start, float ratio, float pan, int len, int atk, int dec, bool reverse)`
  - `bool Grain::active() const`
  - `void Grain::release(int fade_len)` — begin a click-free fade-out from wherever the grain is
  - `void Grain::process(const SampleBuffer& buf, float& outL, float& outR)`

**Background — the three idioms this reuses.** The spec originally expected DUST to establish a grain idiom; DUST shipped as delay taps instead, so this is the engine's first grain code. All three pieces exist elsewhere and are reused rather than re-derived (spec, *Grain math: what to reuse*):

- **Window** — `hann_value_at(x)` is a 192-point sin² table, linearly interpolated, rising 0→1. It is a *half* window, which is exactly right here: the attack is `hann_value_at(i/atk)` and the decay is `hann_value_at(fromEnd/dec)`. Skew comes free from an unequal atk/dec split, so there is no separate skew parameter.
- **Equal-power pan** — the `Voice` idiom (`engine/synth/voice.cpp:89-93`): `a = (pan+1)*0.125` turns, `gr = fast_sin(a)`, `gl = fast_sin(a+0.25)`. Table-based; no `std::sin` in the audio path.
- **Overlap normalization** — `1/sqrt(n)` over active grains, the same law as `SynthEngine::trigger_chord` (`synth_engine.cpp:116`). Applied by the *engine* (Task 3), not by the grain.

- [ ] **Step 1: Write the failing test**

Create `tests/test_grain.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/grain.h"
#include "sampler/sample_buffer.h"
using namespace spky;

static constexpr size_t kCap = 48000;

// A buffer holding one cycle-accurate sine at `hz`, declared as content.
struct SineBuf {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    explicit SineBuf(float hz = 441.f, size_t len = 48000) {
        for (size_t i = 0; i < len; ++i) {
            const float s = std::sin(6.2831853f * hz * float(i) / 48000.f);
            mem[i].l = s;
            mem[i].r = s;
        }
        buf.init(mem.data(), kCap);
        buf.set_rec_size(len);
    }
};

// A buffer whose L channel is a constant 1.0 and R a constant -1.0, so pan
// and window are readable without any signal shape in the way.
struct FlatBuf {
    std::vector<SampleBuffer::Frame> mem{kCap};
    SampleBuffer buf;
    FlatBuf() {
        for (size_t i = 0; i < kCap; ++i) { mem[i].l = 1.f; mem[i].r = -1.f; }
        buf.init(mem.data(), kCap);
        buf.set_rec_size(kCap);
    }
};

static int zero_crossings(const std::vector<float>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++n;
    return n;
}

TEST_CASE("grain: lifetime is exactly len samples, then inactive") {
    FlatBuf f;
    Grain g;
    g.spawn(0.f, 1.f, 0.f, 100, 10, 10, false);
    REQUIRE(g.active());
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(g.active());          // still alive on every one of the 100
        g.process(f.buf, l, r);
    }
    CHECK_FALSE(g.active());
    g.process(f.buf, l, r);            // an inactive grain is silent
    CHECK(l == 0.f);
    CHECK(r == 0.f);
}

TEST_CASE("grain: window rises from silence, holds, and falls to silence") {
    FlatBuf f;
    Grain g;
    const int len = 400, atk = 100, dec = 100;
    g.spawn(0.f, 1.f, 0.f, len, atk, dec, false);

    std::vector<float> env;
    for (int i = 0; i < len; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        env.push_back(l);              // L is a constant 1.0 -> l IS the window * pan gain
    }
    REQUIRE(env.size() == size_t(len));

    // Ends at silence on both sides -- this is the anti-click contract.
    CHECK(env.front() == doctest::Approx(0.f).epsilon(0.001));
    CHECK(env.back()  == doctest::Approx(0.f).epsilon(0.02));
    // Rises monotonically through the attack...
    for (int i = 1; i < atk; ++i) CHECK(env[i] >= env[i - 1]);
    // ...falls monotonically through the decay...
    for (int i = len - dec + 1; i < len; ++i) CHECK(env[i] <= env[i - 1]);
    // ...and genuinely plateaus in between (else "rises then falls" is
    // satisfied by a triangle, which is not the window we specified).
    const float mid = env[len / 2];
    CHECK(mid > env[atk / 2] * 1.5f);
    CHECK(env[atk + 10] == doctest::Approx(mid).epsilon(0.01));
}

TEST_CASE("grain: attack and decay halves are clamped so they never overlap") {
    FlatBuf f;
    Grain g;
    // Ask for halves that sum to more than the grain: they must be scaled
    // down, not allowed to fight. Without the clamp the window goes negative
    // or the plateau inverts.
    g.spawn(0.f, 1.f, 0.f, 100, 90, 90, false);
    float peak = 0.f, lowest = 1e9f;
    for (int i = 0; i < 100; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        if (l > peak) peak = l;
        if (l < lowest) lowest = l;
    }
    CHECK(lowest >= -0.001f);          // never negative
    CHECK(peak > 0.5f);                // still opens properly
    CHECK(peak <= 1.001f);             // and never exceeds unity
}

TEST_CASE("grain: equal-power pan, hard left and hard right") {
    FlatBuf f;
    float l = 0.f, r = 0.f;

    Grain centre;
    centre.spawn(0.f, 1.f, 0.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) centre.process(f.buf, l, r);
    // L is +1 and R is -1 in the buffer, so equal gains means |l| == |r|.
    CHECK(std::fabs(l) == doctest::Approx(std::fabs(r)).epsilon(0.01));
    CHECK(std::fabs(l) == doctest::Approx(0.7071f).epsilon(0.02));

    Grain left;
    left.spawn(0.f, 1.f, -1.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) left.process(f.buf, l, r);
    CHECK(std::fabs(l) > 0.99f);
    CHECK(std::fabs(r) < 0.01f);

    Grain right;
    right.spawn(0.f, 1.f, 1.f, 200, 1, 1, false);
    for (int i = 0; i < 100; ++i) right.process(f.buf, l, r);
    CHECK(std::fabs(l) < 0.01f);
    CHECK(std::fabs(r) > 0.99f);
}

TEST_CASE("grain: ratio 2.0 reads the material an octave up") {
    SineBuf f(441.f);
    Grain g;
    // 12000 samples of output at ratio 1 -> 441 * 0.25 = ~110 crossings.
    Grain base;
    base.spawn(0.f, 1.f, 0.f, 12000, 1, 1, false);
    std::vector<float> v1;
    for (int i = 0; i < 12000; ++i) {
        float l = 0.f, r = 0.f;
        base.process(f.buf, l, r);
        v1.push_back(l);
    }
    const int n1 = zero_crossings(v1);
    CHECK(n1 >= 105);
    CHECK(n1 <= 115);

    g.spawn(0.f, 2.f, 0.f, 12000, 1, 1, false);
    std::vector<float> v2;
    for (int i = 0; i < 12000; ++i) {
        float l = 0.f, r = 0.f;
        g.process(f.buf, l, r);
        v2.push_back(l);
    }
    const int n2 = zero_crossings(v2);
    // Exactly double, within a cycle either way.
    CHECK(n2 >= 2 * n1 - 3);
    CHECK(n2 <= 2 * n1 + 3);
}

TEST_CASE("grain: reverse walks backwards through the material") {
    SineBuf f(441.f);
    // A grain starting at 0 going forwards and one starting at 0 going
    // backwards read different material -- unless reverse is ignored.
    Grain fwd, rev;
    fwd.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, false);
    rev.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, true);
    float diff = 0.f;
    for (int i = 0; i < 2000; ++i) {
        float fl = 0.f, fr = 0.f, rl = 0.f, rr = 0.f;
        fwd.process(f.buf, fl, fr);
        rev.process(f.buf, rl, rr);
        diff += std::fabs(fl - rl);
    }
    CHECK(diff > 100.f);
    // And the reverse grain is real audio, not silence (which would also
    // satisfy the assertion above).
    Grain rev2;
    rev2.spawn(1000.f, 1.f, 0.f, 2000, 1, 1, true);
    float energy = 0.f;
    for (int i = 0; i < 2000; ++i) {
        float l = 0.f, r = 0.f;
        rev2.process(f.buf, l, r);
        energy += l * l;
    }
    CHECK(energy > 100.f);
}

TEST_CASE("grain: release fades out from the current level, click-free") {
    FlatBuf f;
    Grain g;
    g.spawn(0.f, 1.f, 0.f, 10000, 50, 50, false);
    float l = 0.f, r = 0.f;
    for (int i = 0; i < 500; ++i) g.process(f.buf, l, r);   // reach the plateau
    const float before = l;
    REQUIRE(before > 0.6f);

    g.release(200);
    std::vector<float> tail;
    for (int i = 0; i < 200; ++i) {
        g.process(f.buf, l, r);
        tail.push_back(l);
    }
    // No step at the moment of release...
    CHECK(std::fabs(tail.front() - before) < 0.02f);
    // ...monotonic decay...
    for (size_t i = 1; i < tail.size(); ++i) CHECK(tail[i] <= tail[i - 1] + 1e-5f);
    // ...ending in silence, and the grain retires.
    CHECK(tail.back() < 0.02f);
    CHECK_FALSE(g.active());
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add `tests/test_grain.cpp` to the `add_executable(spky_tests` list in `CMakeLists.txt` (no `.cpp` for the engine side — `grain.h` is header-only).

```bash
cmake -S . -B build -G Ninja && cmake --build build
```
Expected: **compile failure**, `fatal error: sampler/grain.h: No such file or directory`.

- [ ] **Step 3: Write the implementation**

Create `engine/sampler/grain.h`:

```cpp
#pragma once
#include "sampler/sample_buffer.h"
#include "fx/fx_util.h"      // hann_value_at
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// One grain of the texture deck's cloud.
//
// Everything is latched at spawn() -- start position, pitch ratio, pan,
// window shape, direction -- so a moving lane makes the cloud audibly drag
// behind rather than smearing every running grain. process() is then pure
// playback with no parameter reads at all.
//
// The window is two halves of the shared rising Hann table (fx_util.h):
// attack over `atk` samples, decay over `dec`, unity in between. An unequal
// split IS the skew control, so ATK/DEC alone shape soft vs percussive.
class Grain {
public:
    // len/atk/dec are in output samples. atk and dec are clamped so they
    // cannot overlap -- a caller asking for more than the grain has gets
    // both halves scaled proportionally, never a fold-over.
    void spawn(float start, float ratio, float pan, int len,
               int atk, int dec, bool reverse) {
        _len     = len < 2 ? 2 : len;
        if (atk < 1) atk = 1;
        if (dec < 1) dec = 1;
        if (atk + dec > _len) {
            // Scale both to fit, keeping their ratio. Integer-safe.
            const long long total = static_cast<long long>(atk) + dec;
            atk = static_cast<int>((static_cast<long long>(atk) * _len) / total);
            dec = _len - atk;
            if (atk < 1) { atk = 1; dec = _len - 1; }
            if (dec < 1) { dec = 1; atk = _len - 1; }
        }
        _atk     = atk;
        _dec     = dec;
        _pos     = start;
        _ratio   = ratio;
        _reverse = reverse;
        _i       = 0;
        _rel_len = 0;
        _rel_ctr = 0;
        _active  = true;

        // Equal-power pan, the Voice idiom (synth/voice.cpp:89-93):
        // angle 0..0.25 turns, gr = sin(a), gl = sin(a + quarter turn).
        const float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.125f;
        _gr = fast_sin(a);
        _gl = fast_sin(a + 0.25f);
    }

    bool active() const { return _active; }
    void kill() { _active = false; }

    // Begin a click-free fade from wherever the window currently is. Used by
    // CHOKE and by leaving FLOW -- the grain must not simply stop.
    void release(int fade_len) {
        if (!_active || _rel_len > 0) return;
        _rel_len = fade_len < 1 ? 1 : fade_len;
        _rel_ctr = _rel_len;
        _rel_from = _window();      // freeze the level we are fading from
    }

    void process(const SampleBuffer& buf, float& outL, float& outR) {
        if (!_active) { outL = 0.f; outR = 0.f; return; }

        float l = 0.f, r = 0.f;
        buf.read_linear(_pos, l, r);

        float w;
        if (_rel_len > 0) {
            // Scale the frozen level by a falling Hann -- continuous at the
            // moment release() was called, and reaching exactly zero.
            w = _rel_from * hann_value_at(static_cast<float>(_rel_ctr)
                                          / static_cast<float>(_rel_len));
            if (_rel_ctr == 0) _active = false;
            else               --_rel_ctr;
        } else {
            w = _window();
        }

        outL = l * w * _gl;
        outR = r * w * _gr;

        _pos += _reverse ? -_ratio : _ratio;
        if (_rel_len == 0 && ++_i >= _len) _active = false;
    }

private:
    float _window() const {
        if (_i < _atk)
            return hann_value_at(static_cast<float>(_i) / static_cast<float>(_atk));
        const int from_end = _len - 1 - _i;
        if (from_end < _dec)
            return hann_value_at(static_cast<float>(from_end) / static_cast<float>(_dec));
        return 1.f;
    }

    float _pos      = 0.f;
    float _ratio    = 1.f;
    float _gl       = 0.7071f;
    float _gr       = 0.7071f;
    float _rel_from = 0.f;
    int   _len      = 0;
    int   _i        = 0;
    int   _atk      = 1;
    int   _dec      = 1;
    int   _rel_len  = 0;
    int   _rel_ctr  = 0;
    bool  _reverse  = false;
    bool  _active   = false;
};

}  // namespace spky
```

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build build && ./build/spky_tests -tc="grain:*"
```
Expected: all 7 cases PASS. Then `./build/spky_tests` — full suite green.

- [ ] **Step 5: Mutation-test every case**

| # | Mutation | Must break |
|---|---|---|
| 1 | In `process`, remove `if (_rel_len == 0 && ++_i >= _len) _active = false;` | *lifetime is exactly len* |
| 2 | In `_window`, `return 1.f;` unconditionally | *window rises from silence* |
| 3 | In `spawn`, delete the `atk + dec > _len` scaling block | *halves are clamped* |
| 4 | In `spawn`, set `_gl = _gr = 0.7071f` ignoring pan | *equal-power pan* |
| 5 | In `process`, `_pos += _reverse ? -1.f : 1.f` (ignore ratio) | *ratio 2.0 reads an octave up* |
| 6 | In `process`, drop the `_reverse` ternary | *reverse walks backwards* |
| 7 | In `release`, set `_rel_from = 1.f` | *release fades from the current level* |

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/grain.h tests/test_grain.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
sampler: Grain -- one latched voice of the cloud

Window, pan and normalization reuse the idioms already in the engine
(fx_util's rising Hann table, Voice's equal-power pan) rather than being
re-derived from src/core/vox.cpp -- DUST was expected to establish these
and shipped as delay taps instead.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: `SamplerEngine` — scheduler, FLOW, STEP, and the material

**Files:**
- Create: `engine/sampler/sampler_engine.h`, `engine/sampler/sampler_engine.cpp`
- Test: `tests/test_sampler_engine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Grain` (Task 2), `SampleBuffer` (Task 1), `spky::Rng` (`engine/mod/rng.h`), `spky::OnePole` (`engine/util/onepole.h`), `IPartEngine` (`engine/parts/engine_iface.h`), `daisysp::Svf` (`Filters/svf.h`).
- Produces (this task; chord/scatter land in Task 4, the voice row in Task 5):
  - `void set_seed(uint32_t)` — **call before `init`**, matching `SynthEngine::set_seed`
  - `void set_memory(SampleBuffer::Frame* buf, size_t frames)` — host injection; **call before `init`**
  - `void init(float sample_rate)`, `void set_targets(const float* t, float tune)`
  - `void trigger(float pitch_norm)`, `void process(float& outL, float& outR)`
  - `void set_flow(bool)`, `void set_hold(bool)`, `void set_gate(bool)`, `void process_in(float, float)`
  - `void set_recording(bool)`, `bool is_recording() const`, `float buffer_fill() const`, `bool is_empty() const`, `size_t rec_size() const`, `void clear()`, `void set_monitor(bool)`
  - `void load_sample(const float* l, const float* r, size_t frames)`
  - `int active_grains() const`, `float grain_len_samples() const`

**Design notes the implementer needs.**

- **The scheduler.** A per-sample countdown `_spawn_ctr`. On zero, one free grain slot is spawned and the counter resets to the spawn interval. Interval = `grain_len / kOverlap` with `kOverlap = 4`, floored at 1 sample — so roughly four grains overlap and the carpet is RMS-continuous, while eight slots leave headroom for scatter. Parameters are read **at spawn**, not per sample: that is what makes the cloud audibly drag behind a moving lane.
- **Normalization.** Per-grain gain `1/sqrt(active)` recomputed at the control tick, never per sample.
- **Fill-follows.** `SOURCE` maps into `[0, buf.rec_size())` — the *current* content length, re-read at every spawn. While recording, that length grows, so grains granulate what has already been captured and never read past the write head into memset-zero memory.
- **STEP.** Grains spawn only while the gate is high, plus `kBurstReleaseS`. Running grains are never cut — they finish their window.
- **`set_hold` (CHOKE)** stops spawning and `release()`s every running grain over one `kRecordFade` (4 ms). Releasing re-arms.
- **Monitoring** adds the dry input at unity to the engine output, which is the part chain's input — hence pre-GRIT, as the spec requires.

- [ ] **Step 1: Write the failing test**

Create `tests/test_sampler_engine.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "sampler/sampler_engine.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kFrames = 48000 * 2;   // 2 s of storage

// An engine with host memory, pre-loaded with a 441 Hz sine as content.
struct Rig {
    std::vector<SampleBuffer::Frame> mem{kFrames};
    std::vector<float> l, r;
    SamplerEngine e;

    explicit Rig(size_t content = 24000, uint32_t seed = 4242) {
        l.resize(content);
        r.resize(content);
        for (size_t i = 0; i < content; ++i) {
            l[i] = std::sin(6.2831853f * 441.f * float(i) / 48000.f);
            r[i] = l[i];
        }
        e.set_seed(seed);
        e.set_memory(mem.data(), kFrames);
        e.init(48000.f);
        e.set_cycle(1.f);
        if (content) e.load_sample(l.data(), r.data(), content);
        feed(0.5f);
    }
    // SOURCE, SIZE, PITCH, MOTION, LEVEL by lane slot.
    void feed(float pitch, float source = 0.f, float size = 0.5f,
              float motion = 0.f, float level = 1.f) {
        float t[LANE_COUNT] = { source, size, pitch, motion, level };
        e.set_targets(t, 0.5f);
    }
    std::vector<float> render(int n) {
        std::vector<float> out(n);
        for (auto& s : out) { float a = 0.f, b = 0.f; e.process(a, b); s = a; }
        return out;
    }
};

static float rms(const std::vector<float>& v, size_t from, size_t n) {
    double acc = 0.0;
    for (size_t i = from; i < from + n && i < v.size(); ++i) acc += double(v[i]) * v[i];
    return float(std::sqrt(acc / double(n)));
}

TEST_CASE("sampler: FLOW is a standing cloud -- RMS never drops out") {
    Rig g;
    g.e.set_flow(true);
    auto v = g.render(48000 * 3);

    const size_t win = 2400;               // 50 ms
    float lowest = 1e9f, highest = 0.f;
    for (size_t i = 4800; i + win < v.size(); i += win) {   // skip the fill-up
        const float e = rms(v, i, win);
        if (e < lowest)  lowest = e;
        if (e > highest) highest = e;
    }
    REQUIRE(highest > 0.02f);              // the cloud actually sounds
    CHECK(lowest > 0.2f * highest);        // ...and never gaps
}

TEST_CASE("sampler: STEP is silent until the gate opens, and tails off after") {
    Rig g;
    g.e.set_flow(false);
    auto pre = g.render(4800);
    for (float s : pre) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));

    g.e.trigger(0.5f);
    g.e.set_gate(true);
    auto on = g.render(9600);
    CHECK(rms(on, 2400, 4800) > 0.02f);

    g.e.set_gate(false);
    auto off = g.render(48000);
    const int rel = int(sampler_cfg::kBurstReleaseS * 48000.f);
    CHECK(rms(off, 0, size_t(rel / 2)) > 0.005f);   // the release still sounds
    CHECK(rms(off, 40000, 4800) < 0.001f);          // a second later, retired
    CHECK(g.e.active_grains() == 0);
}

TEST_CASE("sampler: an empty buffer is silent under FLOW, STEP and gate") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    auto v = g.render(9600);
    for (float s : v) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));
    g.e.set_flow(false);
    g.e.trigger(0.5f);
    g.e.set_gate(true);
    auto w = g.render(9600);
    for (float s : w) CHECK(s == doctest::Approx(0.f).epsilon(0.0001));
}

TEST_CASE("sampler: nullptr memory is silent, not a crash") {
    SamplerEngine e;
    e.set_seed(7);
    e.init(48000.f);                       // set_memory never called
    float t[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 1.f };
    e.set_targets(t, 0.5f);
    e.set_flow(true);
    for (int i = 0; i < 9600; ++i) {
        float a = 1.f, b = 1.f;
        e.process_in(0.5f, 0.5f);
        e.process(a, b);
        CHECK(a == 0.f);
        CHECK(b == 0.f);
    }
    CHECK(e.is_empty());
}

TEST_CASE("sampler: SIZE maps 20 ms .. 2 s exponentially") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() == doctest::Approx(0.02f * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 1.f);
    g.render(200);
    CHECK(g.e.grain_len_samples() == doctest::Approx(2.0f * 48000.f).epsilon(0.02));
    g.feed(0.5f, 0.f, 0.5f);
    g.render(200);
    // 0.02 * 100^0.5 = 0.2 s -- the exponential midpoint, NOT the linear one
    // (~1.01 s). This is the assertion that tells the two mappings apart.
    CHECK(g.e.grain_len_samples() == doctest::Approx(0.2f * 48000.f).epsilon(0.05));
}

TEST_CASE("sampler: grain length is clamped to the content length") {
    Rig g(4800);                           // only 100 ms of content
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 1.f);                // ask for 2 s
    g.render(200);
    CHECK(g.e.grain_len_samples() <= 4800);
}

TEST_CASE("sampler: recording grows the content and the cloud plays while it does") {
    Rig g(0);
    REQUIRE(g.e.is_empty());
    g.e.set_flow(true);
    g.e.set_recording(true);

    std::vector<float> out;
    float heard_early = 0.f;
    for (int i = 0; i < 24000; ++i) {
        const float s = std::sin(6.2831853f * 220.f * float(i) / 48000.f);
        g.e.process_in(s, s);
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        out.push_back(a);
        if (i == 12000) heard_early = rms(out, 6000, 6000);
    }
    g.e.set_recording(false);

    CHECK(g.e.buffer_fill() > 0.f);
    CHECK_FALSE(g.e.is_empty());
    // The sound emerges UNDER the gesture. Without fill-follows this is 0.
    CHECK(heard_early > 0.001f);
}

TEST_CASE("sampler: grains never read past the write head while recording") {
    Rig g(0);
    g.e.set_flow(true);
    g.e.set_recording(true);
    // Record DC 1.0. A read past the write head lands in memset-zero memory,
    // so any such read shows up as a zero inside the recorded region.
    for (int i = 0; i < 24000; ++i) {
        g.e.process_in(1.f, 1.f);
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
    }
    g.e.set_recording(false);
    const size_t n = g.e.rec_size();
    REQUIRE(n > 4800);
    int zeros = 0;
    for (size_t i = sampler_cfg::kRecordFade * 2; i < n - sampler_cfg::kRecordFade * 2; ++i)
        if (std::fabs(g.mem[i].l) < 0.5f) ++zeros;
    CHECK(zeros == 0);
}

TEST_CASE("sampler: monitoring passes the dry input at unity") {
    Rig g(0);
    g.e.set_monitor(true);
    float a = 0.f, b = 0.f;
    g.e.process_in(0.37f, -0.21f);
    g.e.process(a, b);
    CHECK(a == doctest::Approx(0.37f).epsilon(0.001));
    CHECK(b == doctest::Approx(-0.21f).epsilon(0.001));

    g.e.set_monitor(false);
    g.e.process_in(0.37f, -0.21f);
    g.e.process(a, b);
    CHECK(a == doctest::Approx(0.f).epsilon(0.001));
}

TEST_CASE("sampler: CHOKE hold fades the cloud out and re-arms on release") {
    Rig g;
    g.e.set_flow(true);
    auto before = g.render(24000);
    REQUIRE(rms(before, 12000, 4800) > 0.02f);

    g.e.set_hold(true);
    auto during = g.render(9600);
    for (size_t i = 1; i < 500; ++i)       // click-free: no step at release
        CHECK(std::fabs(during[i] - during[i - 1]) < 0.2f);
    CHECK(rms(during, 4800, 4800) < 0.001f);
    CHECK(g.e.active_grains() == 0);

    g.e.set_hold(false);
    auto after = g.render(24000);
    CHECK(rms(after, 12000, 4800) > 0.02f);
}

TEST_CASE("sampler: identical seed and call sequence render bit-identically") {
    Rig a(24000, 31337), b(24000, 31337);
    a.e.set_flow(true);
    b.e.set_flow(true);
    auto va = a.render(48000);
    auto vb = b.render(48000);
    REQUIRE(va.size() == vb.size());
    for (size_t i = 0; i < va.size(); ++i) REQUIRE(va[i] == vb[i]);
}

TEST_CASE("sampler: clear empties the buffer and silences the cloud") {
    Rig g;
    g.e.set_flow(true);
    g.render(4800);
    g.e.clear();
    CHECK(g.e.is_empty());
    CHECK(g.e.buffer_fill() == doctest::Approx(0.f));
    auto v = g.render(24000);
    CHECK(rms(v, 12000, 4800) < 0.001f);
}
```

- [ ] **Step 2: Run the test to verify it fails**

Add to `CMakeLists.txt`:
```cmake
    engine/sampler/sampler_engine.cpp
    tests/test_sampler_engine.cpp
```
```bash
cmake -S . -B build -G Ninja && cmake --build build
```
Expected: **compile failure**, `fatal error: sampler/sampler_engine.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `engine/sampler/sampler_engine.h`. The chord/scatter and voice-row members are declared here already but implemented in Tasks 4 and 5 — declaring them now keeps the header stable across three tasks.

> **Compile note:** `IPartEngine` does not declare `set_gate` or `process_in` until Task 6. For this task, declare those two **without** `override`; Task 6 adds the virtuals and puts `override` back. Do not add the virtuals early — Task 6 owns the neutrality gate that proves adding them changed nothing.

```cpp
#pragma once
#include <cstdint>
#include "mod/rng.h"
#include "parts/engine_iface.h"
#include "sampler/grain.h"
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
#include "util/onepole.h"
#include "Filters/svf.h"      // daisysp::Svf -- the engine's only DaisySP dep

namespace spky {

// The M5 texture deck: a granular cloud behind IPartEngine.
//
// Not a second melodic instrument -- the synth part makes the music, this
// makes the room. Material comes from live input (process_in + set_recording)
// or from a loaded WAV (load_sample); the cloud granulates whatever is
// already captured, including while a recording is still running.
//
// - FLOW: a standing cloud. Grains respawn continuously; the lanes shape the
//   texture and it never gaps.
// - STEP: groove-gated bursts. Grains spawn only while the gate is high plus
//   a short release, so the phrase generator's composed rhythm, DENSITY,
//   CHOKE windows and the GATE jack all chop the texture for free.
class SamplerEngine : public IPartEngine {
public:
    static constexpr int kGrains       = sampler_cfg::kGrains;
    static constexpr int kCtrlInterval = sampler_cfg::kCtrlInterval;
    static constexpr int kMaxChord     = 4;
    // Target overlap in FLOW: the spawn interval is one grain length divided
    // by this, so ~4 grains sound at once and 8 slots leave scatter headroom.
    static constexpr int kOverlap      = 4;

    // Both must be called BEFORE init(), matching SynthEngine::set_seed.
    void set_seed(uint32_t s) { _seed = s; }
    void set_memory(SampleBuffer::Frame* buf, size_t frames) {
        _mem = buf; _mem_frames = buf ? frames : 0;
    }

    // --- IPartEngine ---
    void init(float sample_rate) override;
    void set_targets(const float* t, float tune) override;
    void trigger(float pitch_norm) override;
    void trigger_chord(const float* pitches_norm, int n) override;
    void set_chord(const float* pitches_norm, int n) override;
    void process(float& outL, float& outR) override;
    void set_flow(bool flow) override;
    void set_hold(bool on) override;
    void set_gate(bool on);        // Task 6 adds the virtual + override
    void process_in(float inL, float inR);   // Task 6 adds the virtual + override

    // --- material ---
    void   set_recording(bool on);
    bool   is_recording() const { return _buf.is_recording(); }
    float  buffer_fill() const  { return _buf.fill(); }
    bool   is_empty() const     { return _buf.is_empty(); }
    size_t rec_size() const     { return _buf.rec_size(); }
    void   clear()              { _kill_all(); _buf.clear(); }
    void   set_monitor(bool on) { _monitor = on; }
    void   load_sample(const float* l, const float* r, size_t frames);

    // --- edit layer (Task 5) ---
    void set_tape_mode(bool tape) { _tape = tape; }
    void set_reverse(bool on)     { _reverse = on; }
    void set_feedback(float knob) { _buf.set_feedback(knob); }

    // --- voice row, remapped (Task 5) ---
    void set_window_attack(float n);
    void set_window_decay(float n);
    void set_filt(float n);
    void set_resonance(float n);
    void set_sub(float n);
    void set_detune(float n);

    // --- observation (CSV, tests) ---
    int   active_grains() const;
    float grain_len_samples() const { return _grain_len; }

private:
    void  _update_control();     // recompute derived values on the raster
    void  _spawn_one();          // spawn into a free slot, if any
    void  _kill_all();
    void  _release_all();
    float _next_ratio();         // Task 4: chord round-robin + octave scatter

    SampleBuffer _buf;
    SampleBuffer::Frame* _mem = nullptr;
    size_t _mem_frames = 0;

    Grain _grains[kGrains];
    Rng   _rng;
    uint32_t _seed = 0xC0FFEEu;

    daisysp::Svf _svf_l, _svf_r;
    OnePole _level;

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };
    float _tune = 0.5f;

    // derived at the control tick
    float _grain_len   = 960.f;   // output samples
    float _spawn_every = 240.f;   // samples between spawns
    float _norm        = 1.f;     // 1/sqrt(active)
    float _filt_gain   = 1.f;

    float _spawn_ctr   = 0.f;
    int   _ctrl_ctr    = 0;
    int   _rr          = 0;       // chord round-robin cursor
    int   _release_ctr = 0;       // STEP burst release, in samples

    float _chord[kMaxChord] = { 0.5f, 0.5f, 0.5f, 0.5f };
    int   _chord_n = 1;
    float _burst_pitch   = 0.5f;
    bool  _burst_latched = false;

    float _in_l = 0.f, _in_r = 0.f;

    bool _flow    = false;
    bool _hold    = false;
    bool _gate    = false;
    bool _monitor = false;
    bool _tape    = false;
    bool _reverse = false;

    // voice row (Task 5)
    float _atk_n = 0.3f, _dec_n = 0.3f;
    float _filt_amt = 0.f, _res_n = 0.15f, _sub_n = 0.f, _detune_n = 0.f;
};

}  // namespace spky
```

- [ ] **Step 4: Write the implementation**

Create `engine/sampler/sampler_engine.cpp`. `_next_ratio` returns the plain quantized pitch for now — Task 4 replaces its body; Task 5 fills in the voice-row behaviour behind the setters that already exist here.

```cpp
#include "sampler/sampler_engine.h"
#include "util/math.h"
#include <cmath>

namespace spky {

namespace {
using namespace sampler_cfg;

// Pitch: the lane arrives already quantized from Part. The synth's mapping is
// 110*8^p Hz, so p = 0.5 is unity here and the range is +-18 semitones.
inline float ratio_for(float pitch_norm) {
    return std::pow(8.f, clampf(pitch_norm, 0.f, 1.f) - 0.5f);
}
inline float size_seconds(float n) {
    return kSizeMinS * std::pow(kSizeRange, clampf(n, 0.f, 1.f));
}
inline float cutoff_hz(float n) {
    return kCutoffMinHz * std::pow(kCutoffMaxHz / kCutoffMinHz, clampf(n, 0.f, 1.f));
}
}  // namespace

void SamplerEngine::init(float sample_rate) {
    _sr = sample_rate;
    _buf.init(_mem, _mem_frames);
    _rng.seed(_seed ^ 0x5A11E20Du);

    _svf_l.Init(sample_rate);
    _svf_r.Init(sample_rate);
    _svf_l.SetFreq(kCutoffMaxHz);
    _svf_r.SetFreq(kCutoffMaxHz);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
    _svf_l.SetDrive(0.f);
    _svf_r.SetDrive(0.f);

    _level.init(sample_rate, 0.01f);   // 10 ms, as the synth's LEVEL
    _kill_all();
    _spawn_ctr   = 0.f;
    _ctrl_ctr    = 0;
    _release_ctr = 0;
    _update_control();
}

void SamplerEngine::set_targets(const float* t, float tune) {
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
    _tune = tune;
}

void SamplerEngine::set_flow(bool flow) {
    if (flow == _flow) return;
    _flow = flow;
    if (!_flow) _release_ctr = 0;      // leaving FLOW: running grains decay out
}

void SamplerEngine::set_hold(bool on) {
    if (on == _hold) return;
    _hold = on;
    if (_hold) _release_all();
}

void SamplerEngine::set_gate(bool on) {
    if (on == _gate) return;
    _gate = on;
    if (!on) _release_ctr = static_cast<int>(kBurstReleaseS * _sr);
    else     _burst_latched = false;   // a fresh gate re-latches on next trigger
}

void SamplerEngine::process_in(float inL, float inR) {
    _in_l = inL;
    _in_r = inR;
    _buf.write(inL, inR);              // no-op unless recording
}

void SamplerEngine::set_recording(bool on) { _buf.set_recording(on); }

void SamplerEngine::load_sample(const float* l, const float* r, size_t frames) {
    if (!_buf.valid() || l == nullptr) return;
    _kill_all();
    _buf.clear();
    const size_t n = frames > _buf.capacity() ? _buf.capacity() : frames;
    SampleBuffer::Frame* dst = _buf.raw();
    for (size_t i = 0; i < n; ++i) {
        dst[i].l = l[i];
        dst[i].r = r ? r[i] : l[i];    // mono normals to both channels
    }
    _buf.set_rec_size(n);
}

void SamplerEngine::trigger(float pitch_norm) {
    _burst_pitch   = pitch_norm;
    _burst_latched = true;
    _chord[0] = pitch_norm;
    _chord_n  = 1;
    _rr = 0;
}

void SamplerEngine::trigger_chord(const float* p, int n) {
    if (n < 1) return;
    if (n > kMaxChord) n = kMaxChord;
    for (int i = 0; i < n; ++i) _chord[i] = p[i];
    _chord_n       = n;
    _burst_pitch   = p[0];
    _burst_latched = true;
    _rr = 0;
}

void SamplerEngine::set_chord(const float* p, int n) {
    if (n < 1) return;
    if (n > kMaxChord) n = kMaxChord;
    for (int i = 0; i < n; ++i) _chord[i] = p[i];
    _chord_n = n;
}

int SamplerEngine::active_grains() const {
    int n = 0;
    for (int i = 0; i < kGrains; ++i) if (_grains[i].active()) ++n;
    return n;
}

void SamplerEngine::_kill_all() {
    for (int i = 0; i < kGrains; ++i) _grains[i].kill();
}

void SamplerEngine::_release_all() {
    const int fade = static_cast<int>(kRecordFade);
    for (int i = 0; i < kGrains; ++i)
        if (_grains[i].active()) _grains[i].release(fade);
}

// Task 4 replaces this body with the chord round-robin and octave scatter.
// Until then: the latched burst pitch in STEP, the live lane in FLOW.
float SamplerEngine::_next_ratio() {
    const float p = (!_flow && _burst_latched) ? _burst_pitch : _targets[LANE_PITCH];
    return ratio_for(p);
}

void SamplerEngine::_update_control() {
    // --- SIZE: exponential 20 ms .. 2 s, clamped to what we actually have ---
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    const float content = static_cast<float>(_buf.rec_size());
    if (content > 1.f && len > content) len = content;
    if (len < 2.f) len = 2.f;
    _grain_len = len;

    _spawn_every = _grain_len / static_cast<float>(kOverlap);
    if (_spawn_every < 1.f) _spawn_every = 1.f;

    // --- overlap normalization: 1/sqrt(active), the COLOR loudness law ---
    const int n = active_grains();
    _norm = n > 0 ? 1.f / std::sqrt(static_cast<float>(n)) : 1.f;

    // --- FILT: same bipolar rails as the synth (Task 5 gives it a knob) ---
    const float off   = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
    const float n_raw = _targets[LANE_SIZE] + off;
    _filt_gain = clampf(1.f + n_raw / kFiltFadeRange, 0.f, 1.f);
    const float hz = cutoff_hz(n_raw);
    _svf_l.SetFreq(clampf(hz, 20.f, 0.3f * _sr));
    _svf_r.SetFreq(clampf(hz, 20.f, 0.3f * _sr));
}

void SamplerEngine::_spawn_one() {
    if (_buf.is_empty()) return;

    int slot = -1;
    for (int i = 0; i < kGrains; ++i)
        if (!_grains[i].active()) { slot = i; break; }
    if (slot < 0) return;                       // all busy: skip this spawn

    // Fill-follows: SOURCE maps into the CURRENT content length, so while a
    // recording runs the cloud granulates only what is already captured.
    const float content = static_cast<float>(_buf.rec_size());
    const float centre  = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * content;

    const float ratio = _next_ratio();
    const int   len   = static_cast<int>(_grain_len);
    const int   half  = static_cast<int>(_grain_len * kWindowHalfMin);
    const int   atk   = half < 1 ? 1 : half;

    _grains[slot].spawn(centre, ratio, 0.f, len, atk, atk, _reverse);
}

void SamplerEngine::process(float& outL, float& outR) {
    if (_ctrl_ctr == 0) {
        _ctrl_ctr = kCtrlInterval;
        _update_control();
    }
    --_ctrl_ctr;

    // --- scheduling ---
    const bool spawning = !_hold && (_flow || _gate || _release_ctr > 0);
    if (_release_ctr > 0 && !_gate) --_release_ctr;

    if (spawning) {
        _spawn_ctr -= 1.f;
        if (_spawn_ctr <= 0.f) {
            _spawn_one();
            _spawn_ctr += _spawn_every;
            if (_spawn_ctr < 1.f) _spawn_ctr = 1.f;
        }
    }

    // --- the cloud ---
    float l = 0.f, r = 0.f;
    for (int i = 0; i < kGrains; ++i) {
        if (!_grains[i].active()) continue;
        float gl = 0.f, gr = 0.f;
        _grains[i].process(_buf, gl, gr);
        l += gl;
        r += gr;
    }
    l *= _norm;
    r *= _norm;

    // --- filter, then LEVEL with the FILT silence fade folded in ---
    _svf_l.Process(l);
    _svf_r.Process(r);
    l = _svf_l.Low();
    r = _svf_r.Low();

    const float gain = _level.process(clampf(_targets[LANE_LEVEL], 0.f, 1.f) * _filt_gain);
    l *= gain;
    r *= gain;

    // --- monitoring: dry input at unity, ahead of the part chain (pre-GRIT) ---
    if (_monitor) { l += _in_l; r += _in_r; }

    outL = l;
    outR = r;
}

// --- voice row: behaviour lands in Task 5; the state is stored here ---
void SamplerEngine::set_window_attack(float n) { _atk_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_window_decay(float n)  { _dec_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_filt(float n)          { _filt_amt = clampf(n, -1.f, 1.f); }
void SamplerEngine::set_resonance(float n) {
    _res_n = clampf(n, 0.f, 0.95f);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
}
void SamplerEngine::set_sub(float n)    { _sub_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_detune(float n) { _detune_n = clampf(n, 0.f, 1.f); }

}  // namespace spky
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build build && ./build/spky_tests -tc="sampler:*"
```
Expected: all 12 cases PASS. Then `./build/spky_tests` — full suite green.

If *FLOW is a standing cloud* fails on `lowest > 0.2f * highest`, the spawn interval or the window halves are wrong. **Do not loosen the ratio** — continuity is the property under test.

- [ ] **Step 6: Mutation-test every case**

| # | Mutation | Must break |
|---|---|---|
| 1 | `_spawn_every = _grain_len * 4.f` | *FLOW is a standing cloud* |
| 2 | `const bool spawning = !_hold;` (ignore flow/gate) | *STEP is silent until the gate opens* |
| 3 | Delete the `_buf.is_empty()` guard in `_spawn_one` | *empty buffer is silent* |
| 4 | `size_seconds` returns `kSizeMinS + n * (2.f - kSizeMinS)` (linear) | *SIZE maps exponentially* |
| 5 | Delete the `len > content` clamp | *grain length is clamped* |
| 6 | `_spawn_one` uses `_buf.capacity()` instead of `_buf.rec_size()` | *grains never read past the write head* |
| 7 | Drop the `if (_monitor)` block | *monitoring passes the dry input* |
| 8 | Drop `_release_all()` from `set_hold` | *CHOKE hold fades the cloud out* |
| 9 | Drop the `_kill_all()` from `clear()` | *clear empties the buffer* |

Note: *identical seed renders bit-identically* cannot fail in this task — nothing draws from the Rng yet. It becomes a real gate in Task 4, which re-runs it against a seed mutation. Record this explicitly in the completion notes rather than reporting a passing mutation.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
sampler: SamplerEngine -- scheduler, FLOW, STEP, record and monitor

The cloud granulates the already-captured region while recording runs
(fill-follows), so the sound emerges under the gesture instead of
appearing after it. Chord distribution and MOTION scatter follow.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Chord distribution and the MOTION scatter macro

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp` (`_next_ratio`, `_spawn_one`)
- Test: `tests/test_sampler_engine.cpp` (append)

**Interfaces:**
- Consumes: `_chord[]` / `_chord_n` (already filled by `set_chord`/`trigger_chord` from Task 3), `_rng`.
- Produces: no new public API. Two new observation accessors for the statistical tests:
  - `float last_spawn_ratio() const`, `float last_spawn_pan() const`, `float last_spawn_pos() const`
  - `int spawn_count() const` — so a test can assert its own sample size

**What this task adds.**

- **Chord distribution.** Each spawning grain takes the next chord note round-robin. `COLOR 0` means `_chord_n == 1`, so every grain lands on the root — behaviour identical to the single-note world, which is the bit-identity promise the chord layer made and this must not break.
- **MOTION as one order→chaos axis.** At `MOTION 0`: all grains tight on the SOURCE point, stereo-centred, regular spawn timing. At `MOTION 1`: position jitter up to `±kScatterPosFrac` of content length, full stereo spread, spawn-interval jitter of `±kScatterTimeFrac`, and a mild octave scatter on chord notes (`kScatterOctProb`). Every draw comes from the part's seeded `Rng`, so the statistics are exact and reproducible, never flaky.

> **Draw order is part of the contract.** The Rng is a single stream; changing the *order* of draws inside `_spawn_one` changes every rendered sample. Draw exactly: position, pan, octave, then timing. If a later task needs another draw, it appends — it never inserts.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sampler_engine.cpp`:

```cpp
// --- Task 4: chord distribution and scatter -------------------------------

// Collect the spawn parameters of `want` grains under the current settings.
struct SpawnStats {
    std::vector<float> ratio, pan, pos;
};
static SpawnStats collect(Rig& g, int want) {
    SpawnStats s;
    int last = g.e.spawn_count();
    int guard = 0;
    while (int(s.ratio.size()) < want && guard++ < 4000000) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (g.e.spawn_count() != last) {
            last = g.e.spawn_count();
            s.ratio.push_back(g.e.last_spawn_ratio());
            s.pan.push_back(g.e.last_spawn_pan());
            s.pos.push_back(g.e.last_spawn_pos());
        }
    }
    return s;
}

TEST_CASE("sampler: COLOR 0 puts every grain on the root") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.2f, 0.f);          // MOTION 0 -> no octave scatter
    const float root[1] = { 0.6f };
    g.e.set_chord(root, 1);

    auto s = collect(g, 200);
    REQUIRE(s.ratio.size() == 200);        // the sample size is a precondition
    const float expect = std::pow(8.f, 0.6f - 0.5f);
    for (float rr : s.ratio) CHECK(rr == doctest::Approx(expect).epsilon(0.001));
}

TEST_CASE("sampler: a chord spreads grains round-robin over its notes") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.2f, 0.f);          // MOTION 0: no octave scatter
    const float chord[4] = { 0.40f, 0.50f, 0.60f, 0.70f };
    g.e.set_chord(chord, 4);

    auto s = collect(g, 400);
    REQUIRE(s.ratio.size() == 400);

    // Every ratio must be one of the four chord ratios...
    int hits[4] = { 0, 0, 0, 0 };
    for (float rr : s.ratio) {
        bool matched = false;
        for (int i = 0; i < 4; ++i) {
            const float want = std::pow(8.f, chord[i] - 0.5f);
            if (std::fabs(rr - want) < 0.001f) { ++hits[i]; matched = true; }
        }
        CHECK(matched);
    }
    // ...and all four must be covered roughly evenly (round-robin, not
    // "always the root", which would still satisfy the check above).
    for (int i = 0; i < 4; ++i) {
        CHECK(hits[i] > 60);
        CHECK(hits[i] < 140);
    }
}

TEST_CASE("sampler: MOTION 0 is tight and centred; MOTION 1 spreads") {
    Rig tight;
    tight.e.set_flow(true);
    tight.feed(0.5f, 0.5f, 0.2f, 0.f);     // MOTION 0
    auto a = collect(tight, 300);
    REQUIRE(a.pos.size() == 300);

    // Position: every grain on the same point.
    float pmin = 1e9f, pmax = -1e9f;
    for (float p : a.pos) { if (p < pmin) pmin = p; if (p > pmax) pmax = p; }
    CHECK(pmax - pmin < 1.f);
    // Pan: dead centre.
    for (float p : a.pan) CHECK(std::fabs(p) < 0.001f);

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.5f, 0.2f, 1.f);      // MOTION 1
    auto b = collect(wide, 300);
    REQUIRE(b.pos.size() == 300);

    float qmin = 1e9f, qmax = -1e9f;
    for (float p : b.pos) { if (p < qmin) qmin = p; if (p > qmax) qmax = p; }
    const float content = 24000.f;
    // Spread is real...
    CHECK(qmax - qmin > 0.5f * sampler_cfg::kScatterPosFrac * content);
    // ...and bounded by the spec range (plus a little slack for the fold).
    CHECK(qmax - qmin < 2.2f * sampler_cfg::kScatterPosFrac * content);

    float panmin = 1e9f, panmax = -1e9f;
    for (float p : b.pan) { if (p < panmin) panmin = p; if (p > panmax) panmax = p; }
    CHECK(panmin < -0.7f);
    CHECK(panmax > 0.7f);
}

TEST_CASE("sampler: MOTION 1 scatters some chord notes an octave away") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.5f, 0.2f, 1.f);         // MOTION 1
    const float chord[3] = { 0.45f, 0.55f, 0.65f };
    g.e.set_chord(chord, 3);

    auto s = collect(g, 600);
    REQUIRE(s.ratio.size() == 600);

    int octaves = 0, plain = 0;
    for (float rr : s.ratio) {
        for (int i = 0; i < 3; ++i) {
            const float base = std::pow(8.f, chord[i] - 0.5f);
            if (std::fabs(rr - base) < 0.001f) ++plain;
            else if (std::fabs(rr - base * 2.f) < 0.002f ||
                     std::fabs(rr - base * 0.5f) < 0.002f) ++octaves;
        }
    }
    CHECK(plain + octaves == 600);          // nothing lands off the chord
    // The probability is kScatterOctProb; with 600 draws the binomial band is
    // tight enough to assert both that it happens and that it stays a spice.
    CHECK(octaves > 600 * sampler_cfg::kScatterOctProb * 0.5f);
    CHECK(octaves < 600 * sampler_cfg::kScatterOctProb * 1.6f);
}

TEST_CASE("sampler: MOTION 1 jitters the spawn timing, MOTION 0 does not") {
    // Regular spawn timing at MOTION 0 means the gaps between spawns are all
    // equal. This test reads the gaps directly rather than inferring them.
    auto gaps = [](Rig& g, int want) {
        std::vector<int> out;
        int last_count = g.e.spawn_count(), since = 0;
        while (int(out.size()) < want) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            ++since;
            if (g.e.spawn_count() != last_count) {
                last_count = g.e.spawn_count();
                out.push_back(since);
                since = 0;
            }
        }
        return out;
    };

    Rig tight;
    tight.e.set_flow(true);
    tight.feed(0.5f, 0.f, 0.3f, 0.f);
    auto a = gaps(tight, 60);
    a.erase(a.begin());                     // the first gap is the fill-up
    int amin = 1 << 30, amax = 0;
    for (int v : a) { if (v < amin) amin = v; if (v > amax) amax = v; }
    CHECK(amax - amin <= 1);                // regular, within rounding

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.f, 0.3f, 1.f);
    auto b = gaps(wide, 60);
    b.erase(b.begin());
    int bmin = 1 << 30, bmax = 0;
    for (int v : b) { if (v < bmin) bmin = v; if (v > bmax) bmax = v; }
    CHECK(bmax - bmin > 3);                 // genuinely jittered
}

TEST_CASE("sampler: a different seed changes the scattered render") {
    Rig a(24000, 31337), b(24000, 999);
    a.e.set_flow(true);
    b.e.set_flow(true);
    a.feed(0.5f, 0.5f, 0.2f, 1.f);
    b.feed(0.5f, 0.5f, 0.2f, 1.f);
    auto va = a.render(48000);
    auto vb = b.render(48000);
    float diff = 0.f;
    for (size_t i = 0; i < va.size(); ++i) diff += std::fabs(va[i] - vb[i]);
    CHECK(diff > 1.f);
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build
```
Expected: **compile failure** — `spawn_count`, `last_spawn_ratio`, `last_spawn_pan`, `last_spawn_pos` are not members.

- [ ] **Step 3: Add the observation accessors to the header**

In `engine/sampler/sampler_engine.h`, in the observation block:

```cpp
    // --- observation (CSV, tests) ---
    int   active_grains() const;
    float grain_len_samples() const { return _grain_len; }
    int   spawn_count() const       { return _spawn_count; }
    float last_spawn_ratio() const  { return _last_ratio; }
    float last_spawn_pan() const    { return _last_pan; }
    float last_spawn_pos() const    { return _last_pos; }
```

...and in the private state:

```cpp
    int   _spawn_count = 0;
    float _last_ratio  = 1.f;
    float _last_pan    = 0.f;
    float _last_pos    = 0.f;
```

- [ ] **Step 4: Replace `_next_ratio` and `_spawn_one`**

In `engine/sampler/sampler_engine.cpp`, replace both functions:

```cpp
// Round-robin over the current chord. COLOR 0 leaves _chord_n == 1, so every
// grain lands on the root and the single-note world is untouched -- the
// chord layer's bit-identity promise, carried into the cloud.
//
// MOTION adds a mild octave scatter on top: at full scatter, kScatterOctProb
// of grains jump an octave up or down. The draw happens for every spawn
// regardless of MOTION so the Rng stream does not change shape with the knob.
float SamplerEngine::_next_ratio() {
    const bool  latched = (!_flow && _burst_latched);
    const float motion  = clampf(_targets[LANE_MOTION], 0.f, 1.f);

    float p;
    if (latched && _chord_n <= 1) {
        p = _burst_pitch;
    } else {
        p = _chord[_rr % _chord_n];
        _rr = (_rr + 1) % _chord_n;
    }

    float ratio = ratio_for(p);

    const float roll = _rng.next_unipolar();
    if (motion * kScatterOctProb > roll)
        ratio *= _rng.next_unipolar() < 0.5f ? 0.5f : 2.f;

    return ratio;
}

void SamplerEngine::_spawn_one() {
    if (_buf.is_empty()) return;

    int slot = -1;
    for (int i = 0; i < kGrains; ++i)
        if (!_grains[i].active()) { slot = i; break; }
    if (slot < 0) return;                       // all busy: skip this spawn

    const float motion  = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    // Fill-follows: SOURCE maps into the CURRENT content length, so while a
    // recording runs the cloud granulates only what is already captured.
    const float content = static_cast<float>(_buf.rec_size());

    // --- Rng draw order is contract: position, pan, octave, timing. ---
    const float jitter = _rng.next_bipolar() * motion * kScatterPosFrac * content;
    float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * content + jitter;
    while (centre >= content) centre -= content;
    while (centre < 0.f)      centre += content;

    const float pan = _rng.next_bipolar() * motion;

    const float ratio = _next_ratio();          // draws the octave roll

    const int len  = static_cast<int>(_grain_len);
    const int half = static_cast<int>(_grain_len * kWindowHalfMin);
    const int atk  = half < 1 ? 1 : half;

    _grains[slot].spawn(centre, ratio, pan, len, atk, atk, _reverse);

    // Spawn-timing jitter, applied to the NEXT interval. Drawn last.
    _spawn_jitter = _rng.next_bipolar() * motion * kScatterTimeFrac;

    _last_ratio = ratio;
    _last_pan   = pan;
    _last_pos   = centre;
    ++_spawn_count;
}
```

Add `float _spawn_jitter = 0.f;` to the private state, and apply it in `process`:

```cpp
        if (_spawn_ctr <= 0.f) {
            _spawn_one();
            _spawn_ctr += _spawn_every * (1.f + _spawn_jitter);
            if (_spawn_ctr < 1.f) _spawn_ctr = 1.f;
        }
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build && ./build/spky_tests -tc="sampler:*"
```
Expected: all 18 cases PASS (12 from Task 3 + 6 new). Then the full suite.

- [ ] **Step 6: Mutation-test**

| # | Mutation | Must break |
|---|---|---|
| 1 | `_next_ratio` never advances `_rr` | *chord spreads round-robin* |
| 2 | `_next_ratio` ignores `_chord`, always uses `_targets[LANE_PITCH]` | *COLOR 0 puts every grain on the root* (it will drift off the set root) |
| 3 | Drop `* motion` from the position jitter | *MOTION 0 is tight and centred* |
| 4 | Drop `* motion` from the pan draw | *MOTION 0 is tight and centred* |
| 5 | `motion * kScatterOctProb > roll` → `false` | *MOTION 1 scatters some chord notes* |
| 6 | `motion * kScatterOctProb > roll` → `true` | *MOTION 1 scatters some chord notes* (upper bound) |
| 7 | Drop `_spawn_jitter` from the interval | *MOTION 1 jitters the spawn timing* |
| 8 | `_rng.seed(0x1u)` in `init` (ignore `_seed`) | *a different seed changes the scattered render* |

Mutation 8 is the one Task 3 could not run. Confirm it now.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp
git commit -m "$(cat <<'EOF'
sampler: chord round-robin and MOTION as one order-to-chaos axis

COLOR 0 keeps every grain on the root, so the single-note world is
unchanged. MOTION 0 is a tight centred loop; MOTION 1 opens position,
stereo, spawn timing and a mild octave scatter, all from the seeded Rng
so the statistics are exact rather than flaky.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Tape/Digital, Reverse, and the remapped voice row

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp` (`_spawn_one`, `_update_control`)
- Test: `tests/test_sampler_engine.cpp` (append)

**Interfaces:**
- Consumes: `_tape`, `_reverse`, `_atk_n`, `_dec_n`, `_sub_n`, `_detune_n`, `_filt_amt`, `_res_n` — all already stored by the Task 3 setters.
- Produces: no new public API. One accessor for the tape test: `int last_spawn_len() const`.

**The two knob families this fills in.**

**Tape vs Digital** — a cloud has no playhead, so the old slice-mode distinction becomes a grain property:
- *Digital* (default): fixed output duration `SIZE`; the grain reads `SIZE · ratio` worth of material. Texture speed is independent of pitch.
- *Tape*: the grain covers a fixed `SIZE` **of material** and therefore lasts `SIZE / ratio` — low notes become long smearing grains, high notes short and fleeting.

**The voice row, remapped** — every printed label stays true for both engines:

| Knob | Sampler meaning |
|---|---|
| ATK / DEC | grain-window attack/decay halves (soft ↔ percussive); DEC additionally scales the STEP burst release |
| FILT / RES | one stereo `Svf` on the cloud output, same bipolar FILT semantics — full left fades to silence, full right opens to 14 kHz (already wired in Task 3); RES is its resonance |
| SUB | share of grains spawning an octave down |
| DTUN | per-grain detune spread in cents |

> **Rng draw order, extended.** Task 4 fixed the order as position, pan, octave, timing. SUB and DTUN append **after** timing: position, pan, octave, timing, sub, detune. Do not insert them earlier — every rendered sample would change.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_sampler_engine.cpp`:

```cpp
// --- Task 5: tape/digital, reverse, voice row -----------------------------

TEST_CASE("sampler: Digital holds the grain length; Tape scales it by 1/ratio") {
    Rig g;
    g.e.set_flow(true);
    g.feed(0.5f, 0.f, 0.3f, 0.f);          // MOTION 0: no octave scatter
    const float up[1] = { 0.5f + 1.f / 6.f };   // +6 semitones -> ratio ~1.414
    g.e.set_chord(up, 1);

    g.e.set_tape_mode(false);
    auto d = collect(g, 40);
    REQUIRE(d.ratio.size() == 40);
    const int digital_len = g.e.last_spawn_len();

    g.e.set_tape_mode(true);
    auto t = collect(g, 40);
    REQUIRE(t.ratio.size() == 40);
    const int tape_len = g.e.last_spawn_len();

    const float ratio = g.e.last_spawn_ratio();
    REQUIRE(ratio > 1.3f);                  // the precondition the test rests on
    CHECK(tape_len == doctest::Approx(float(digital_len) / ratio).epsilon(0.05));

    // And at unity ratio the two modes agree (else "scales by 1/ratio" could
    // be any constant scaling).
    const float unity[1] = { 0.5f };
    g.e.set_chord(unity, 1);
    g.e.set_tape_mode(false);
    collect(g, 20);
    const int d2 = g.e.last_spawn_len();
    g.e.set_tape_mode(true);
    collect(g, 20);
    CHECK(g.e.last_spawn_len() == doctest::Approx(float(d2)).epsilon(0.05));
}

TEST_CASE("sampler: ATK and DEC shape the window from soft to percussive") {
    auto peak_at = [](Rig& g, float atk, float dec, int probe) {
        g.e.set_window_attack(atk);
        g.e.set_window_decay(dec);
        g.e.set_flow(true);
        g.feed(0.5f, 0.f, 0.5f, 0.f);
        return g.render(probe);
    };
    // A long attack must reach a lower level early in the grain than a short
    // one. Measured on the first 20 ms of a fresh cloud.
    Rig soft, hard;
    auto s = peak_at(soft, 1.f, 1.f, 960);
    auto h = peak_at(hard, 0.f, 0.f, 960);
    float se = 0.f, he = 0.f;
    for (int i = 0; i < 480; ++i) { se += std::fabs(s[i]); he += std::fabs(h[i]); }
    CHECK(he > se * 1.3f);
}

TEST_CASE("sampler: SUB sends a share of grains an octave down") {
    Rig none;
    none.e.set_flow(true);
    none.feed(0.5f, 0.f, 0.2f, 0.f);
    const float root[1] = { 0.5f };
    none.e.set_chord(root, 1);
    none.e.set_sub(0.f);
    auto a = collect(none, 300);
    REQUIRE(a.ratio.size() == 300);
    for (float rr : a.ratio) CHECK(rr == doctest::Approx(1.f).epsilon(0.001));

    Rig half;
    half.e.set_flow(true);
    half.feed(0.5f, 0.f, 0.2f, 0.f);
    half.e.set_chord(root, 1);
    half.e.set_sub(0.5f);
    auto b = collect(half, 400);
    REQUIRE(b.ratio.size() == 400);
    int down = 0;
    for (float rr : b.ratio) if (rr < 0.75f) ++down;
    CHECK(down > 400 * 0.5f * 0.6f);
    CHECK(down < 400 * 0.5f * 1.4f);
}

TEST_CASE("sampler: DTUN spreads grain ratios in cents, 0 is exact") {
    Rig none;
    none.e.set_flow(true);
    none.feed(0.5f, 0.f, 0.2f, 0.f);
    const float root[1] = { 0.5f };
    none.e.set_chord(root, 1);
    none.e.set_detune(0.f);
    auto a = collect(none, 200);
    REQUIRE(a.ratio.size() == 200);
    for (float rr : a.ratio) CHECK(rr == doctest::Approx(1.f).epsilon(0.0005));

    Rig wide;
    wide.e.set_flow(true);
    wide.feed(0.5f, 0.f, 0.2f, 0.f);
    wide.e.set_chord(root, 1);
    wide.e.set_detune(1.f);
    auto b = collect(wide, 300);
    REQUIRE(b.ratio.size() == 300);
    float lo = 1e9f, hi = -1e9f;
    for (float rr : b.ratio) { if (rr < lo) lo = rr; if (rr > hi) hi = rr; }
    // +-35 cents -> ratio band of 2^(70/1200) ~= 1.041 wide.
    const float band = std::pow(2.f, 2.f * sampler_cfg::kDetuneCeilCt / 1200.f);
    CHECK(hi / lo > band * 0.7f);
    CHECK(hi / lo < band * 1.15f);
}

TEST_CASE("sampler: FILT full left fades to silence at ANY lane position") {
    // The FILT invariant, mirrored from the synth (tests/test_filt.cpp).
    for (float lane : { 0.f, 0.25f, 0.5f, 0.75f, 1.f }) {
        Rig g;
        g.e.set_flow(true);
        g.feed(0.5f, 0.f, lane, 0.f);       // lane == the FILTER/SIZE slot
        g.e.set_filt(-1.f);
        auto v = g.render(24000);
        CHECK(rms(v, 12000, 4800) < 0.001f);
    }
    // ...and full right is emphatically not silent, at the same lane values.
    Rig open;
    open.e.set_flow(true);
    open.feed(0.5f, 0.f, 0.5f, 0.f);
    open.e.set_filt(1.f);
    auto v = open.render(24000);
    CHECK(rms(v, 12000, 4800) > 0.01f);
}

TEST_CASE("sampler: Reverse plays the material backwards") {
    Rig fwd(24000, 555), rev(24000, 555);
    fwd.e.set_flow(true);
    rev.e.set_flow(true);
    rev.e.set_reverse(true);
    auto a = fwd.render(24000);
    auto b = rev.render(24000);
    float diff = 0.f, energy = 0.f;
    for (size_t i = 0; i < a.size(); ++i) {
        diff   += std::fabs(a[i] - b[i]);
        energy += std::fabs(b[i]);
    }
    CHECK(diff > 10.f);                     // genuinely different...
    CHECK(energy > 10.f);                   // ...and not just silent
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build
```
Expected: **compile failure** — `last_spawn_len` is not a member. After adding it, the tape, SUB, DTUN and ATK/DEC cases fail on assertions.

- [ ] **Step 3: Implement**

Add to the header's observation block and private state:

```cpp
    int last_spawn_len() const { return _last_len; }
```
```cpp
    int _last_len = 0;
```

Replace the tail of `_spawn_one` (from the `ratio` line onward):

```cpp
    const float ratio_base = _next_ratio();     // draws the octave roll

    // Spawn-timing jitter, applied to the NEXT interval. Drawn 4th.
    _spawn_jitter = _rng.next_bipolar() * motion * kScatterTimeFrac;

    // SUB: a share of grains an octave down. Drawn 5th.
    float ratio = ratio_base;
    if (_rng.next_unipolar() < _sub_n * kSubMaxShare) ratio *= 0.5f;

    // DTUN: per-grain detune spread, +-kDetuneCeilCt at full. Drawn 6th.
    const float cents = _rng.next_bipolar() * _detune_n * kDetuneCeilCt;
    if (cents != 0.f) ratio *= std::pow(2.f, cents / 1200.f);

    // Tape: the grain covers a fixed SIZE OF MATERIAL and so lasts
    // SIZE / ratio -- low notes smear long, high notes are fleeting.
    // Digital: fixed output duration; the grain reads SIZE * ratio.
    float lenf = _tape ? _grain_len / (ratio > 0.001f ? ratio : 0.001f)
                       : _grain_len;
    if (lenf < 2.f) lenf = 2.f;
    const int len = static_cast<int>(lenf);

    // ATK/DEC: the two window halves, each from kWindowHalfMin to
    // kWindowHalfMax of the grain. An unequal split IS the skew control.
    const float atk_f = lerpf(kWindowHalfMin, kWindowHalfMax, _atk_n);
    const float dec_f = lerpf(kWindowHalfMin, kWindowHalfMax, _dec_n);
    int atk = static_cast<int>(lenf * atk_f);
    int dec = static_cast<int>(lenf * dec_f);
    if (atk < 1) atk = 1;
    if (dec < 1) dec = 1;

    _grains[slot].spawn(centre, ratio, pan, len, atk, dec, _reverse);

    _last_ratio = ratio;
    _last_pan   = pan;
    _last_pos   = centre;
    _last_len   = len;
    ++_spawn_count;
```

And make DEC scale the STEP burst release, in `set_gate`:

```cpp
void SamplerEngine::set_gate(bool on) {
    if (on == _gate) return;
    _gate = on;
    if (!on) {
        // DEC stretches the burst tail: a long decay leaves a longer trail
        // after the composed note ends (spec, voice-row table).
        const float rel = kBurstReleaseS * (0.5f + 1.5f * _dec_n);
        _release_ctr = static_cast<int>(rel * _sr);
    } else {
        _burst_latched = false;
    }
}
```

- [ ] **Step 4: Run the tests**

```bash
cmake --build build && ./build/spky_tests -tc="sampler:*"
```
Expected: all 24 cases PASS. Then the full suite.

> If *FLOW is a standing cloud* (Task 3) now fails, the ATK/DEC defaults are opening the window halves so wide that the plateau vanished and overlap dropped. Fix the **defaults** (`_atk_n`/`_dec_n`), not the continuity assertion.

- [ ] **Step 5: Mutation-test**

| # | Mutation | Must break |
|---|---|---|
| 1 | `lenf = _grain_len` unconditionally (ignore `_tape`) | *Digital holds… Tape scales* |
| 2 | `atk_f`/`dec_f` fixed at `kWindowHalfMin` | *ATK and DEC shape the window* |
| 3 | Drop the SUB branch | *SUB sends a share an octave down* |
| 4 | `if (_rng.next_unipolar() < 1.f)` in the SUB branch | *SUB…* (upper bound) |
| 5 | Drop the DTUN multiply | *DTUN spreads grain ratios* |
| 6 | `_filt_gain = 1.f` in `_update_control` | *FILT full left fades to silence* |
| 7 | Drop `_reverse` from the `spawn` call | *Reverse plays the material backwards* |

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp
git commit -m "$(cat <<'EOF'
sampler: Tape/Digital, Reverse and the remapped voice row

No dead knobs: ATK/DEC shape the grain window, FILT/RES drive one stereo
Svf with the synth's bipolar semantics (full left still fades to silence
at any lane position), SUB is the octave-down share, DTUN the per-grain
detune spread. Every printed label stays true for both engines.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: Wire it into `Part` and `Instrument` — and prove the synth did not move

**Files:**
- Modify: `engine/parts/engine_iface.h`, `engine/parts/part.h`, `engine/parts/part.cpp`
- Modify: `engine/instrument.h`, `engine/instrument.cpp`
- Modify: `CMakeLists.txt`, `host/vcv/Makefile`
- Test: `tests/test_sampler_part.cpp`
- Modify: `engine/sampler/sampler_engine.h` (restore `override` on the two virtuals)

**Interfaces:**
- Consumes: everything from Tasks 1–5.
- Produces:
  - `EngineId::ENGINE_SAMPLER = 2`
  - `virtual void IPartEngine::process_in(float, float) {}` and `virtual void IPartEngine::set_gate(bool) {}`
  - `FxMem` gains `SampleBuffer::Frame* sampler_buf[PART_COUNT]` and `size_t sampler_frames`
  - `Part::process(float inL, float inR, float& outL, float& outR, float& sendL, float& sendR)` plus the two existing overloads, unchanged in signature
  - `Instrument`: `sampler_record(int p, bool)`, `sampler_clear(int p)`, `sampler_fill(int p)`, `sampler_monitor(int p, bool)`, `load_sample(int p, const float* l, const float* r, size_t frames)`, `sampler_speed_mode(int p, bool tape)`, `sampler_reverse(int p, bool)`, `sampler_feedback(int p, float)`, `sampler_grains(int p)`

**This is the task the neutrality gate belongs to.** Everything here touches shared code that the melodic path runs through. The gate is Step 7 and it is not optional.

- [ ] **Step 1: Write the failing test**

Create `tests/test_sampler_part.cpp`:

```cpp
#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "instrument.h"
#include "sampler/sampler_config.h"
using namespace spky;

static constexpr size_t kSFrames = 48000;   // 1 s per part is plenty for tests

struct InstRig {
    std::vector<float> echo[PART_COUNT][2];
    std::vector<SampleBuffer::Frame> sbuf[PART_COUNT];
    AmbientReverb reverb;
    FxMem mem;
    Instrument inst;

    InstRig() {
        for (int p = 0; p < PART_COUNT; ++p) {
            for (int c = 0; c < 2; ++c) {
                echo[p][c].assign(Flux::kMaxSamples, 0.f);
                mem.echo[p][c] = echo[p][c].data();
            }
            sbuf[p].assign(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
            mem.sampler_buf[p] = sbuf[p].data();
        }
        mem.sampler_frames = kSFrames;
        mem.reverb = &reverb;
        inst.init(48000.f, mem);
    }
    // Render n samples with a constant input on both channels.
    std::vector<float> render(int n, float in = 0.f) {
        std::vector<float> out(n);
        for (int i = 0; i < n; ++i) {
            float l = 0.f, r = 0.f;
            inst.process(&in, &in, &l, &r, 1);
            out[i] = l;
        }
        return out;
    }
};

static float rms_of(const std::vector<float>& v, size_t from, size_t n) {
    double acc = 0.0;
    for (size_t i = from; i < from + n && i < v.size(); ++i) acc += double(v[i]) * v[i];
    return float(std::sqrt(acc / double(n)));
}

TEST_CASE("part: ENGINE_SAMPLER is selectable and records from the input") {
    InstRig g;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    // The click-free swap needs a few ms to complete.
    g.render(960);
    CHECK(g.inst.engine_id(PART_A) == ENGINE_SAMPLER);

    REQUIRE(g.inst.sampler_fill(PART_A) == doctest::Approx(0.f));
    g.inst.sampler_record(PART_A, true);
    // Feed a DC 0.5 in; the buffer must fill.
    g.render(24000, 0.5f);
    g.inst.sampler_record(PART_A, false);
    g.render(480);
    CHECK(g.inst.sampler_fill(PART_A) > 0.4f);
}

TEST_CASE("part: input reaches ONLY the sampler engine, and only when routed") {
    InstRig g;
    // Part B stays on the synth. Recording is armed on A only.
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    g.inst.sampler_record(PART_A, true);
    g.render(9600, 0.5f);
    g.inst.sampler_record(PART_A, false);
    CHECK(g.inst.sampler_fill(PART_A) > 0.f);
    CHECK(g.inst.sampler_fill(PART_B) == doctest::Approx(0.f));
}

TEST_CASE("part: a nullptr sampler buffer leaves the part silent, not crashing") {
    std::vector<float> echo[PART_COUNT][2];
    AmbientReverb reverb;
    FxMem mem;
    for (int p = 0; p < PART_COUNT; ++p)
        for (int c = 0; c < 2; ++c) {
            echo[p][c].assign(Flux::kMaxSamples, 0.f);
            mem.echo[p][c] = echo[p][c].data();
        }
    mem.reverb = &reverb;                       // sampler_buf left nullptr
    Instrument inst;
    inst.init(48000.f, mem);
    inst.set_engine(PART_A, ENGINE_SAMPLER);
    inst.set_engine(PART_B, ENGINE_SAMPLER);
    const float in = 0.5f;
    for (int i = 0; i < 24000; ++i) {
        float l = 0.f, r = 0.f;
        inst.process(&in, &in, &l, &r, 1);
        REQUIRE(std::isfinite(l));
        REQUIRE(std::isfinite(r));
    }
    CHECK(inst.sampler_fill(PART_A) == doctest::Approx(0.f));
}

TEST_CASE("part: the composed gate reaches the sampler in STEP") {
    InstRig g;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    // Load content so the cloud has something to play.
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i)
        tone[i] = std::sin(6.2831853f * 300.f * float(i) / 48000.f);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());

    g.inst.set_step(PART_A, true, 8);
    g.inst.set_target_base(PART_A, LANE_LEVEL, 1.f);
    auto v = g.render(48000 * 4);
    // A gated texture is neither silent nor continuous: it has loud and
    // quiet stretches. Measure the spread of 50 ms RMS windows.
    float lo = 1e9f, hi = 0.f;
    for (size_t i = 4800; i + 2400 < v.size(); i += 2400) {
        const float e = rms_of(v, i, 2400);
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > 0.005f);          // it sounds
    CHECK(lo < 0.4f * hi);       // ...and it is chopped, not a standing cloud
}

TEST_CASE("part: engine switch synth <-> sampler is click-free") {
    InstRig g;
    std::vector<float> tone(24000);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = 0.8f;
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());
    auto a = g.render(9600);

    g.inst.set_engine(PART_A, ENGINE_SYNTH);
    auto b = g.render(9600);
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    auto c = g.render(9600);

    // No step anywhere across either swap.
    for (size_t i = 1; i < b.size(); ++i) CHECK(std::fabs(b[i] - b[i - 1]) < 0.25f);
    for (size_t i = 1; i < c.size(); ++i) CHECK(std::fabs(c[i] - c[i - 1]) < 0.25f);
}

TEST_CASE("part: voice-row edits stick on the sampler while the synth is active") {
    InstRig g;
    // Set FILT hard left while the SYNTH is the active engine...
    g.inst.set_voice_filt(PART_A, -1.f);
    g.inst.set_engine(PART_A, ENGINE_SAMPLER);
    g.render(960);
    std::vector<float> tone(24000, 0.8f);
    g.inst.load_sample(PART_A, tone.data(), tone.data(), tone.size());
    g.inst.set_target_base(PART_A, LANE_LEVEL, 1.f);
    auto v = g.render(24000);
    // ...and the sampler must already be silent when it comes up.
    CHECK(rms_of(v, 12000, 4800) < 0.005f);
}

TEST_CASE("part: CHOKE holds a sampler drone like a synth drone") {
    InstRig g;
    g.inst.set_engine(PART_B, ENGINE_SAMPLER);
    g.render(960);
    std::vector<float> tone(24000, 0.6f);
    g.inst.load_sample(PART_B, tone.data(), tone.data(), tone.size());
    g.inst.set_target_base(PART_B, LANE_LEVEL, 1.f);
    // Full A priority: B must duck whenever A holds.
    g.inst.set_choke(-1.f);
    auto v = g.render(48000 * 3);
    float lo = 1e9f, hi = 0.f;
    for (size_t i = 4800; i + 2400 < v.size(); i += 2400) {
        const float e = rms_of(v, i, 2400);
        if (e < lo) lo = e;
        if (e > hi) hi = e;
    }
    CHECK(hi > 0.005f);
    CHECK(lo < 0.6f * hi);       // the cloud really ducks
}
```

- [ ] **Step 2: Run to verify failure**

Add `tests/test_sampler_part.cpp` to `CMakeLists.txt`, then:
```bash
cmake -S . -B build -G Ninja && cmake --build build
```
Expected: **compile failure** — `ENGINE_SAMPLER` is not a member of `EngineId`.

- [ ] **Step 3: Extend the engine interface**

In `engine/parts/engine_iface.h`, replace the enum and add the two virtuals next to the existing M2 no-ops:

```cpp
// Selectable part engines. ENGINE_SYNTH is the boot default from M2 on;
// the test tone stays selectable (tests, A/B reference). M5 adds the
// sampler -- appended, so no persisted id changes meaning.
enum EngineId { ENGINE_TEST_TONE = 0, ENGINE_SYNTH = 1, ENGINE_SAMPLER = 2 };
```

```cpp
    // M5 additions -- default no-ops, so the synth and the test tone are
    // untouched by them (the neutrality gate proves this).
    //
    // process_in: per-sample input feed, called by Part BEFORE process().
    // Only the sampler implements it -- it records and monitors from here.
    virtual void process_in(float /*inL*/, float /*inR*/) {}
    //
    // set_gate: Part's composed gate signal (the manual 5 ms pulse OR'd with
    // the groove's note sustain -- exactly what Part::gate() computes), so a
    // cloud can sound for the composed note duration in STEP. Forwarded on
    // EDGES, not per sample (the set_cycle idiom). The synth ignores it: it
    // has its own envelope.
    virtual void set_gate(bool /*on*/) {}
```

Then restore `override` on both declarations in `engine/sampler/sampler_engine.h`:
```cpp
    void set_gate(bool on) override;
    void process_in(float inL, float inR) override;
```

- [ ] **Step 4: Wire `Part`**

`engine/parts/part.h` — add the include, the member, extend `init`, switch `_engine_for`, dual-forward the voice row, and declare the new `process`:

```cpp
#include "sampler/sampler_engine.h"
```

```cpp
    void init(float sample_rate, uint32_t seed_base,
              float* echo_l = nullptr, float* echo_r = nullptr,
              SampleBuffer::Frame* sampler_mem = nullptr, size_t sampler_frames = 0);
```

```cpp
    // VOICE edit layer - forwarded to BOTH engines directly, so edits stick
    // whichever engine is active. The sampler reinterprets each knob as its
    // cloud analogue (spec: "no dead knobs"), so one panel row serves both.
    void set_voice_attack(float n)    { _synth.set_attack(n);    _sampler.set_window_attack(n); }
    void set_voice_decay(float n)     { _synth.set_decay(n);     _sampler.set_window_decay(n); }
    void set_voice_resonance(float n) { _synth.set_resonance(n); _sampler.set_resonance(n); }
    void set_voice_sub(float n)       { _synth.set_sub(n);       _sampler.set_sub(n); }
    void set_voice_detune(float n)    { _synth.set_detune(n);    _sampler.set_detune(n); }
    void set_voice_filt(float t)      { _synth.set_filt(t);      _sampler.set_filt(t); }

    SamplerEngine& sampler() { return _sampler; }
```

```cpp
    // The input-carrying form is the real one; the two legacy overloads feed
    // silence, which keeps every existing caller and test bit-identical.
    void process(float inL, float inR, float& outL, float& outR,
                 float& sendL, float& sendR);
    void process(float& outL, float& outR, float& sendL, float& sendR) {
        process(0.f, 0.f, outL, outR, sendL, sendR);
    }
    void process(float& outL, float& outR) {
        float sl, sr;
        process(0.f, 0.f, outL, outR, sl, sr);
    }
```

Private state and dispatch:
```cpp
    SamplerEngine _sampler;
    bool          _last_gate = false;
```
```cpp
    IPartEngine* _engine_for(EngineId e) {
        switch (e) {
            case ENGINE_SYNTH:   return static_cast<IPartEngine*>(&_synth);
            case ENGINE_SAMPLER: return static_cast<IPartEngine*>(&_sampler);
            default:             return static_cast<IPartEngine*>(&_tone);
        }
    }
```

`engine/parts/part.cpp` — in `init`, seed and hand over the memory (order matters: both setters precede `init`, matching `SynthEngine`):

```cpp
    _sampler.set_seed(seed_base ^ 0x5A11E20Du);
    _sampler.set_memory(sampler_mem, sampler_frames);
    _sampler.init(sample_rate);
```

In `process`, change the signature and add three things — the gate edge, the input feed, and the swap re-sync:

```cpp
void Part::process(float inL, float inR, float& outL, float& outR,
                   float& sendL, float& sendR) {
```

In the swap block, next to the existing `set_flow`/`set_hold` re-sync:
```cpp
        _engine->set_gate(_last_gate);   // the freshly swapped-in engine
```

And immediately before `_engine->process(outL, outR);`:
```cpp
    // Composed gate, forwarded on edges only (see engine_iface.h). Computed
    // after _gate_ctr has been advanced, so it reflects THIS sample.
    const bool g = gate();
    if (g != _last_gate) {
        _last_gate = g;
        _engine->set_gate(g);
    }

    _engine->process_in(inL, inR);
    _engine->process(outL, outR);
```

- [ ] **Step 5: Wire `Instrument`**

`engine/instrument.h` — extend `FxMem` and add the sampler API next to the M2 voice API:

```cpp
#include "sampler/sampler_engine.h"
```
```cpp
struct FxMem {
    float* echo[PART_COUNT][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    AmbientReverb* reverb = nullptr;
    // M5 texture deck: one stereo record buffer per part. Spec sizing is
    // 42 s at 48 kHz (~16 MB/part) -- hosts allocate on the heap (desktop,
    // Rack) or in SDRAM (M6). nullptr -> that part's sampler runs silent.
    SampleBuffer::Frame* sampler_buf[PART_COUNT] = { nullptr, nullptr };
    size_t sampler_frames = 0;
};
```
```cpp
    // --- M5 sampler API (spec "Instrument API") ---
    void sampler_record(int p, bool on) {
        _parts[p].sampler().set_recording(on);
        // Monitoring follows REC automatically, in one place, so both hosts
        // get it right (plan: deliberate deviation 3).
        _parts[p].sampler().set_monitor(on);
    }
    void  sampler_clear(int p)              { _parts[p].sampler().clear(); }
    float sampler_fill(int p) const         { return _parts[p].sampler().buffer_fill(); }
    bool  sampler_empty(int p) const        { return _parts[p].sampler().is_empty(); }
    void  sampler_monitor(int p, bool on)   { _parts[p].sampler().set_monitor(on); }
    int   sampler_grains(int p) const       { return _parts[p].sampler().active_grains(); }
    void  sampler_speed_mode(int p, bool tape) { _parts[p].sampler().set_tape_mode(tape); }
    void  sampler_reverse(int p, bool on)   { _parts[p].sampler().set_reverse(on); }
    void  sampler_feedback(int p, float n)  { _parts[p].sampler().set_feedback(n); }
    void  load_sample(int p, const float* l, const float* r, size_t frames) {
        _parts[p].sampler().load_sample(l, r, frames);
    }
```

`engine/instrument.cpp` — pass the memory through `init`:
```cpp
    _parts[PART_A].init(sample_rate, 0x1234abcdu,
                        mem.echo[PART_A][0], mem.echo[PART_A][1],
                        mem.sampler_buf[PART_A], mem.sampler_frames);
    _parts[PART_B].init(sample_rate, 0x9e3779b9u,
                        mem.echo[PART_B][0], mem.echo[PART_B][1],
                        mem.sampler_buf[PART_B], mem.sampler_frames);
```

...and thread the inputs in `process`. Un-comment the parameter names and feed both parts the same sample:
```cpp
void Instrument::process(const float* inL, const float* inR,
                         float* outL, float* outR, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const float in_l = inL ? inL[i] : 0.f;
        const float in_r = inR ? inR[i] : 0.f;
```
Then at the two `_parts[...].process(...)` calls, prepend the inputs:
```cpp
        _parts[pri].process(in_l, in_r, pl[pri], prr[pri], psl[pri], psr[pri]);
```
```cpp
        _parts[yld].process(in_l, in_r, pl[yld], prr[yld], psl[yld], psr[yld]);
```

- [ ] **Step 6: Register the new sources in both build files**

`CMakeLists.txt` — already done in Tasks 1 and 3. `host/vcv/Makefile` — add to the explicit engine list (`Makefile:32-46`), which the comment there says must stay identical to CMake's:
```make
	$(REPO)/engine/sampler/sample_buffer.cpp \
	$(REPO)/engine/sampler/sampler_engine.cpp
```

Then:
```bash
cmake --build build && ./build/spky_tests -tc="part:*"
```
Expected: all 7 new cases PASS. Then `./build/spky_tests` — full suite green, **no existing test edited**.

- [ ] **Step 7: The synth neutrality gate — run it, do not skip it**

This is the counterpart of the COLOR-0 gate. Adding two virtuals, an enum value, a `Part` member and an input path must be a **byte-level no-op** for a part on `ENGINE_SYNTH`.

First capture the baseline from the commit before this task:
```bash
git stash push -- engine host CMakeLists.txt
cmake --build build
./build/render.exe host/render/scenarios/dorian_vs_drift.json /tmp/base_a.wav /tmp/base_a.csv
./build/render.exe host/render/scenarios/rhythm_groove.json  /tmp/base_b.wav /tmp/base_b.csv
git stash pop
```
(If either scenario name does not exist, use two from `ls host/render/scenarios/` that exercise the melodic path, and record which ones in the completion notes.)

Then re-render with the M5 code in and compare:
```bash
cmake --build build
./build/render.exe host/render/scenarios/dorian_vs_drift.json /tmp/new_a.wav /tmp/new_a.csv
./build/render.exe host/render/scenarios/rhythm_groove.json  /tmp/new_b.wav /tmp/new_b.csv
cmp /tmp/base_a.wav /tmp/new_a.wav && cmp /tmp/base_b.wav /tmp/new_b.wav && echo "NEUTRAL"
```
Expected output: `NEUTRAL`.

**If the files differ, stop and report.** Do not adjust a tolerance — there is none. The likely causes, in order: `Part::process` computing the gate before `_gate_ctr` is advanced (changing nothing audible but reordering nothing either — check it really is a no-op), a `_control_tick` call moved, or `_sampler` being constructed in a way that perturbs the part's Rng stream. The sampler has its own `Rng`; if it ever draws from the part's, that is the bug.

Also confirm the double-render determinism gate still holds:
```bash
./build/render.exe host/render/scenarios/dorian_vs_drift.json /tmp/det.wav /tmp/det.csv
cmp /tmp/new_a.wav /tmp/det.wav && echo "DETERMINISTIC"
```

- [ ] **Step 8: Mutation-test**

| # | Mutation | Must break |
|---|---|---|
| 1 | `_engine_for` returns `&_synth` for `ENGINE_SAMPLER` | *ENGINE_SAMPLER is selectable* |
| 2 | Drop the `_engine->process_in(inL, inR)` call | *ENGINE_SAMPLER … records from the input* |
| 3 | Feed `0.f, 0.f` instead of `in_l, in_r` in `Instrument::process` | *ENGINE_SAMPLER … records from the input* |
| 4 | Drop the gate edge block from `Part::process` | *the composed gate reaches the sampler in STEP* |
| 5 | Drop `_engine->set_gate(_last_gate)` from the swap block | *engine switch is click-free* (or the gate test, depending on ordering — record which) |
| 6 | `set_voice_filt` forwards to `_synth` only | *voice-row edits stick on the sampler* |
| 7 | Pass `nullptr, 0` for the sampler memory in `Instrument::init` | *ENGINE_SAMPLER … records from the input* |

- [ ] **Step 9: Commit**

```bash
git add engine/parts/engine_iface.h engine/parts/part.h engine/parts/part.cpp \
        engine/instrument.h engine/instrument.cpp engine/sampler/sampler_engine.h \
        tests/test_sampler_part.cpp CMakeLists.txt host/vcv/Makefile
git commit -m "$(cat <<'EOF'
part: ENGINE_SAMPLER, input threading and the composed gate

Instrument's inL/inR pointers were in the signature and discarded since
M1; they now reach the parts. The gate is forwarded on edges rather than
per sample, matching the set_cycle idiom. Voice-row setters reach both
engines so the panel row stays 1:1 across them.

Synth neutrality verified: pinned baseline scenarios render byte-identical.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: The render host — live-recording scenarios end to end

**Files:**
- Create: `host/render/wav_reader.h`
- Modify: `host/render/scenario.h`, `host/render/scenario.cpp`, `host/render/main.cpp`
- Create: `host/render/scenarios/sampler_texture_deck.json`
- Test: `tests/test_wav.cpp` (append a reader round-trip case)

**Interfaces:**
- Consumes: `WavWriter` (`host/render/wav_writer.h`) as the format reference; the `Instrument` sampler API from Task 6.
- Produces:
  - `struct WavData { int sample_rate; std::vector<float> l, r; };`
  - `bool read_wav(const std::string& path, WavData& out, std::string& err);`
  - `Scenario::input_wav` (a top-level JSON string field)
  - scenario actions: `load_wav`, `sampler_record`, `sampler_clear`, `sampler_monitor`, `sampler_speed_mode`, `sampler_reverse`, `sampler_feedback`, `set_engine` extended with `"sampler"`
  - two new CSV columns per part: `a_fill`, `a_grains`, `b_fill`, `b_grains`

> **The writer is not a reader.** `wav_writer.h` emits a fixed 16-bit PCM header with no extra chunks. Real files carry `LIST`/`fact` chunks before `data`, and come in 16/24/32-bit PCM and 32-bit float. The reader must walk the chunk list rather than assume a fixed offset, and must handle mono by normalling it to both channels. Anything it cannot parse is an error with a message, never a silent zero-length load.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_wav.cpp`:

```cpp
#include "render/wav_reader.h"

TEST_CASE("wav: writer output round-trips through the reader") {
    const std::string path = "test_roundtrip.wav";
    {
        spky::WavWriter w(48000);
        for (int i = 0; i < 1000; ++i) {
            const float s = std::sin(6.2831853f * 440.f * float(i) / 48000.f);
            w.push(s, -s);
        }
        REQUIRE(w.write(path));
    }
    spky::WavData d;
    std::string err;
    REQUIRE(spky::read_wav(path, d, err));
    CHECK(err.empty());
    CHECK(d.sample_rate == 48000);
    REQUIRE(d.l.size() == 1000);
    REQUIRE(d.r.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        const float s = std::sin(6.2831853f * 440.f * float(i) / 48000.f);
        // 16-bit quantization is the only loss.
        CHECK(d.l[i] == doctest::Approx(s).epsilon(0.001));
        CHECK(d.r[i] == doctest::Approx(-s).epsilon(0.001));
    }
    std::remove(path.c_str());
}

TEST_CASE("wav: a missing or malformed file is an error, not an empty load") {
    spky::WavData d;
    std::string err;
    CHECK_FALSE(spky::read_wav("definitely_not_here.wav", d, err));
    CHECK_FALSE(err.empty());

    const std::string path = "test_garbage.wav";
    { FILE* f = std::fopen(path.c_str(), "wb"); std::fputs("NOTARIFF", f); std::fclose(f); }
    err.clear();
    CHECK_FALSE(spky::read_wav(path, d, err));
    CHECK_FALSE(err.empty());
    std::remove(path.c_str());
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build
```
Expected: **compile failure**, `fatal error: render/wav_reader.h: No such file or directory`.

(`tests/test_wav.cpp` already builds; confirm `host` is on the test target's include path — `CMakeLists.txt:118` shows `render` gets `host engine third_party`. If `spky_tests` does not, add `host` to its `target_include_directories`.)

- [ ] **Step 3: Write the reader**

Create `host/render/wav_reader.h`:

```cpp
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Counterpart to wav_writer.h. Unlike the writer -- which emits one fixed
// header shape -- this walks the chunk list, because real files put LIST and
// fact chunks before data, and accepts 16/24/32-bit PCM and 32-bit float.
namespace spky {

struct WavData {
    int sample_rate = 48000;
    std::vector<float> l, r;
};

inline bool read_wav(const std::string& path, WavData& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }

    struct Closer { FILE* f; ~Closer() { std::fclose(f); } } closer{ f };

    auto rd_u32 = [&](uint32_t& v) {
        uint8_t b[4];
        if (std::fread(b, 1, 4, f) != 4) return false;
        v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
        return true;
    };
    auto rd_u16 = [&](uint16_t& v) {
        uint8_t b[2];
        if (std::fread(b, 1, 2, f) != 2) return false;
        v = uint16_t(uint16_t(b[0]) | (uint16_t(b[1]) << 8));
        return true;
    };

    char tag[4];
    uint32_t riff_size = 0;
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "RIFF", 4) != 0) {
        err = "not a RIFF file: " + path; return false;
    }
    if (!rd_u32(riff_size)) { err = "truncated header"; return false; }
    if (std::fread(tag, 1, 4, f) != 4 || std::memcmp(tag, "WAVE", 4) != 0) {
        err = "not a WAVE file: " + path; return false;
    }

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    bool have_fmt = false;

    while (true) {
        char cid[4];
        uint32_t csize = 0;
        if (std::fread(cid, 1, 4, f) != 4) break;      // clean EOF
        if (!rd_u32(csize)) break;

        if (std::memcmp(cid, "fmt ", 4) == 0) {
            uint16_t block = 0;
            uint32_t byterate = 0;
            if (!rd_u16(fmt) || !rd_u16(channels) || !rd_u32(rate) ||
                !rd_u32(byterate) || !rd_u16(block) || !rd_u16(bits)) {
                err = "truncated fmt chunk"; return false;
            }
            have_fmt = true;
            // Skip any fmt extension (WAVE_FORMAT_EXTENSIBLE carries one).
            if (csize > 16) std::fseek(f, long(csize - 16), SEEK_CUR);
        } else if (std::memcmp(cid, "data", 4) == 0) {
            if (!have_fmt) { err = "data chunk before fmt chunk"; return false; }
            if (channels < 1 || channels > 2) {
                err = "unsupported channel count"; return false;
            }
            const int bytes = bits / 8;
            if (bytes < 2 || bytes > 4) { err = "unsupported bit depth"; return false; }

            const size_t frames = size_t(csize) / (size_t(bytes) * channels);
            out.sample_rate = int(rate);
            out.l.resize(frames);
            out.r.resize(frames);

            std::vector<uint8_t> raw(csize);
            if (std::fread(raw.data(), 1, csize, f) != csize) {
                err = "truncated data chunk"; return false;
            }
            const uint8_t* p = raw.data();
            for (size_t i = 0; i < frames; ++i) {
                for (int c = 0; c < channels; ++c) {
                    float v = 0.f;
                    if (fmt == 3 && bytes == 4) {           // IEEE float
                        float tmp;
                        std::memcpy(&tmp, p, 4);
                        v = tmp;
                    } else if (bytes == 2) {
                        int16_t s = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
                        v = float(s) / 32768.f;
                    } else if (bytes == 3) {
                        int32_t s = (int32_t(p[0]) << 8) | (int32_t(p[1]) << 16) |
                                    (int32_t(p[2]) << 24);
                        v = float(s >> 8) / 8388608.f;
                    } else {                                 // 32-bit PCM
                        int32_t s = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
                        v = float(s) / 2147483648.f;
                    }
                    if (c == 0) out.l[i] = v;
                    else        out.r[i] = v;
                    p += bytes;
                }
                if (channels == 1) out.r[i] = out.l[i];      // mono normals
            }
            return true;
        } else {
            std::fseek(f, long(csize + (csize & 1)), SEEK_CUR);   // chunks pad to even
        }
    }
    err = "no data chunk in " + path;
    return false;
}

}  // namespace spky
```

- [ ] **Step 4: Extend the scenario layer**

`host/render/scenario.h` — one new field:
```cpp
struct Scenario {
    int    sample_rate = 48000;
    float  bpm = 120.f;
    double duration_s = 10.0;
    std::string input_wav;            // fed into Instrument::process inputs
    std::vector<Event> init_events;
    std::vector<Event> events;
};
```

`host/render/scenario.cpp` — parse it next to the other top-level fields:
```cpp
    out.input_wav = j.value("input_wav", std::string());
```

...extend `parse_engine`:
```cpp
static EngineId parse_engine(const std::string& s) {
    if (s == "test_tone") return ENGINE_TEST_TONE;
    if (s == "sampler")   return ENGINE_SAMPLER;
    return ENGINE_SYNTH;
}
```

...and add the actions to the dispatch chain (`load_wav` needs the reader):
```cpp
    else if (a == "sampler_record")     inst.sampler_record(e.part, e.flag);
    else if (a == "sampler_clear")      inst.sampler_clear(e.part);
    else if (a == "sampler_monitor")    inst.sampler_monitor(e.part, e.flag);
    else if (a == "sampler_speed_mode") inst.sampler_speed_mode(e.part, e.svalue == "tape");
    else if (a == "sampler_reverse")    inst.sampler_reverse(e.part, e.flag);
    else if (a == "sampler_feedback")   inst.sampler_feedback(e.part, e.value);
    else if (a == "load_wav") {
        WavData d;
        std::string err;
        if (read_wav(e.svalue, d, err))
            inst.load_sample(e.part, d.l.data(), d.r.data(), d.l.size());
        else
            std::fprintf(stderr, "load_wav: %s\n", err.c_str());
    }
```
Add `#include "render/wav_reader.h"` and `#include <cstdio>` at the top of `scenario.cpp`.

- [ ] **Step 5: Feed the input and extend the CSV in `main.cpp`**

Load the input file once, before the loop:
```cpp
    WavData in_wav;
    if (!scen.input_wav.empty()) {
        std::string err;
        if (!read_wav(scen.input_wav, in_wav, err)) {
            std::printf("input_wav: %s\n", err.c_str());
            return 4;
        }
    }
```

In the render loop, replace the `nullptr, nullptr` call:
```cpp
        const float in_l = i < in_wav.l.size() ? in_wav.l[i] : 0.f;
        const float in_r = i < in_wav.r.size() ? in_wav.r[i] : 0.f;
        float l = 0.f, r = 0.f;
        inst.process(&in_l, &in_r, &l, &r, 1);
```

CSV — extend the header literal (two new names per part, appended to each part's block so the existing column order is untouched):
```cpp
    std::fprintf(csv, "t,"
        "a_src,a_size,a_pitch,a_motion,a_level,a_pcv,a_gate,"
        "a_fx0,a_fx1,a_fx2,a_fx3,a_fx4,a_voices,a_v0,a_v1,a_v2,a_v3,a_pgate,a_fill,a_grains,"
        "b_src,b_size,b_pitch,b_motion,b_level,b_pcv,b_gate,"
        "b_fx0,b_fx1,b_fx2,b_fx3,b_fx4,b_voices,b_v0,b_v1,b_v2,b_v3,b_pgate,b_fill,b_grains,"
        "morph,couple,drift,weather,phase_err\n");
```
...and the loop body, right after the `pitch_gate` write:
```cpp
        std::fprintf(csv, ",%.4f,%d", inst.sampler_fill(p), inst.sampler_grains(p));
```

- [ ] **Step 6: Write the acceptance scenario**

Create `host/render/scenarios/sampler_texture_deck.json`. Part A is a synth phrase with COLOR; part B is the sampler, recording live from `input_wav`, then a FLOW cloud that MOTION opens from tight loop to fog, then a STEP-chopped texture at the end.

```json
{
  "_comment": "M5 acceptance demo: synth phrase + live-recorded texture deck",
  "sample_rate": 48000,
  "bpm": 96,
  "duration_s": 40.0,
  "input_wav": "host/render/scenarios/assets/in_drone.wav",
  "init": [
    { "action": "set_engine", "part": 1, "value": "sampler" },
    { "action": "set_color", "part": 0, "value": 0.6 },
    { "action": "set_target_base", "part": 1, "slot": 4, "value": 1.0 },
    { "action": "set_target_base", "part": 1, "slot": 3, "value": 0.0 },
    { "action": "set_target_active", "part": 1, "slot": 3, "flag": false },
    { "action": "sampler_feedback", "part": 1, "value": 0.95 }
  ],
  "events": [
    { "t": 1.0,  "action": "sampler_record", "part": 1, "flag": true },
    { "t": 9.0,  "action": "sampler_record", "part": 1, "flag": false },
    { "t": 10.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.0 },
    { "t": 16.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.35 },
    { "t": 22.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.7 },
    { "t": 28.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 1.0 },
    { "t": 32.0, "action": "set_step", "part": 1, "flag": true, "ivalue": 8 },
    { "t": 32.0, "action": "set_target_base", "part": 1, "slot": 3, "value": 0.5 }
  ]
}
```

The scenario needs an input file. Generate one from the existing renderer rather than committing binary audio if `assets/` does not already exist — record the choice in the completion notes:
```bash
mkdir -p host/render/scenarios/assets
./build/render.exe host/render/scenarios/dorian_vs_drift.json \
                   host/render/scenarios/assets/in_drone.wav /tmp/ignore.csv
```

- [ ] **Step 7: Run everything**

```bash
cmake --build build && ./build/spky_tests
./build/render.exe host/render/scenarios/sampler_texture_deck.json /tmp/m5.wav /tmp/m5.csv
```
Expected: full suite green; the render succeeds and `/tmp/m5.csv` shows `b_fill` rising from 0 between t=1 and t=9, `b_grains` non-zero afterwards.

Determinism gate:
```bash
./build/render.exe host/render/scenarios/sampler_texture_deck.json /tmp/m5b.wav /tmp/m5b.csv
cmp /tmp/m5.wav /tmp/m5b.wav && echo "DETERMINISTIC"
```

Neutrality gate, again (Task 7 touched `Instrument::process`'s call site):
```bash
./build/render.exe host/render/scenarios/dorian_vs_drift.json /tmp/neu.wav /tmp/neu.csv
cmp /tmp/new_a.wav /tmp/neu.wav && echo "STILL NEUTRAL"
```

- [ ] **Step 8: Mutation-test**

| # | Mutation | Must break |
|---|---|---|
| 1 | `read_wav` assumes data starts at byte 44 | *writer output round-trips* (only if the writer emits extra chunks — if it does not, add a `LIST` chunk to the test file by hand and record that) |
| 2 | `read_wav` returns `true` on a missing file | *a missing or malformed file is an error* |
| 3 | `read_wav` ignores `channels == 1` normalling | add a mono file to the round-trip case and confirm |

- [ ] **Step 9: Commit**

```bash
git add host/render/wav_reader.h host/render/scenario.h host/render/scenario.cpp \
        host/render/main.cpp host/render/scenarios/sampler_texture_deck.json \
        tests/test_wav.cpp CMakeLists.txt
git commit -m "$(cat <<'EOF'
render: WAV reader, input_wav and the sampler scenario actions

Live-recording scenarios now render end to end: input_wav feeds the
process inputs, load_wav loads material from disk, and the CSV carries
per-part fill and grain count. The reader walks the chunk list rather
than assuming the writer's fixed header offset.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## After the plan

**Verify before claiming M5a done:**
1. `./build/spky_tests` — full suite green, no existing test edited.
2. Synth neutrality: two pinned baseline scenarios byte-identical (Task 6, Step 7).
3. Determinism: double render of the acceptance scenario byte-identical.
4. Every task's mutation table run and reported.

**Then, and only then:**
- Listen to `/tmp/m5.wav`. The spec's acceptance demo is a *sound*, not a test — MOTION should audibly open from a tight loop to fog, and the STEP section should chop.
- Measure the CPU cost with the host bench and record it in the milestone notes as a checkpoint. The 5.3× SRAM-vs-SDRAM figure in the spec is a directional floor-risk number from a 64 KB window, not this deck's cost; the bench's grain-read proxy was replaced by the taps workload in 2.7.0, so reproducing it means restoring a grain proxy.
- Update `docs/roadmap.md`: M5 moves from Planned to Done, engine + render host, VCV deferred to M5b.
- **M5b (VCV host) is planned separately**, against the `Instrument` API this plan produced: ENG remap to Synth↔Sampler with the test tone moving to the context menu, the REC pad + LED through `gen_panel.py` (appended after `ROT_B`, `PART_STRIDE` untouched, `res/test_panel.py`'s frozen `PARAM_ORDER` extended by hand), `dr_wav` vendored for load/save, `dataToJson`/`dataFromJson` built from scratch, the factory sample and its autoload, and the Rack play-test checklist.

**Known open questions to answer during implementation** (from the spec's own list, plus two this plan found):
- `Buffer` desktop portability — confirmed at header level; verify at link time when `sample_buffer.cpp` first builds (Task 1).
- Grain spawn scheduling at control-tick granularity is fine for SIZE ≥ 20 ms — verify no audible combing at minimum SIZE (listening, after Task 5).
- `set_gate` must not double-trigger with `trigger` in STEP: trigger latches, gate sustains. Assert exactly one burst per fired step (Task 6).
- **New:** the VCV host reinitialises the whole instrument on any sample-rate change (`Spotymod.cpp:228-231`), which will wipe a loaded sample. M5b must decide: re-load from the persisted path, or resample. Not an M5a concern, but do not lose the finding.
- **New:** two 42 s stereo buffers are ~32 MB. The VCV module already holds ~3.8 MB of echo *by value*; the sampler buffers must go on the heap, as the spec's "desktop/Rack: heap" says, not follow the echo idiom. M5b.
