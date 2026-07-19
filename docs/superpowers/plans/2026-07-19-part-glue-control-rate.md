# Part Glue to Control Rate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop `Part::process` from recomputing, 96 times per block, target values that `SynthEngine` reads once per block — cutting the largest attributed line in the CPU budget (112 820 cycles per part, ≈23 % of budget for both).

**Architecture:** The rasterable work in `Part::process` moves into a private `_control_tick()`. Task 1 extracts it and still calls it every sample — a pure move, proven by a byte-identical render. Task 4 gates that call behind a 96-sample counter owned by `Part` and phase-aligned with `SynthEngine`'s own, with an event refresh on a PITCH-lane fire. Task 6 then pulls the remaining per-sample consumers (LEVEL, the five FX targets, the `set_targets` push) onto the same tick, which is where the render legitimately changes.

**Tech Stack:** C++17, header-mostly engine under `engine/`, doctest under `tests/`, CMake + Ninja + clang on the desktop side, `host/render` CLI for WAV renders, `bench/run.py` for on-hardware cycle counts.

**Spec:** `docs/superpowers/specs/2026-07-19-part-glue-control-rate-design.md`

## Global Constraints

- Build environment: `source env.sh` before any cmake/ctest invocation. Clang + Ninja, no MSVC.
- The engine is no-heap and no-RNG-in-audio-path. Do not introduce allocation, `std::vector`, or exceptions in `engine/`.
- Do not touch `engine/mod/lane.cpp`, `engine/mod/super_modulator.cpp`, or anything under `lib/` (vendored DaisySP). The mod-plane spec owns lane semantics and its ruling stands: `SuperModulator::process()` keeps running every sample.
- Raster constant is `SynthEngine::kCtrlInterval` (= 96, `engine/synth/synth_engine.h:33`). Do not define a second 96 in `Part`.
- Commit trailer on every commit: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- Branch: `cpu-hunt`.
- Bit-exactness is NOT required (Bastian, 2026-07-19): pre-release engine, parallel CPU work landing. No checksum gates any task. Tasks 1-5 should still be behaviour-neutral by construction; Task 6 changes the render on purpose.
- Another session (session_01T727NNiydTvBtBBxpZ3oDG) also commits to `cpu-hunt`, in `engine/fx/{flux,comp}.*` and `third_party/oliverb/`. Stage only your own explicit paths -- never `git add -A` or `git add .`.

---

### Task 0: Branch

**Files:** none

- [ ] **Step 1: Create the branch**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
git checkout -b cpu-hunt
git status --short
```

Expected: branch created, clean tree.

---

### Task 1: Extract `_control_tick()`, still called every sample

The pure move. No raster yet. This isolates "did I transcribe the block correctly" from "did the raster change behaviour", so that when Task 4 breaks something you know which of the two it was.

One deliberate reordering happens here: `_engine->set_targets(...)` moves from *before* the chord block to *after* it. This is safe by inspection and the Task 3 gate proves it empirically. `SynthEngine::set_targets` (`engine/synth/synth_engine.cpp:67`) touches the chord only via `if (_chord_n <= 1) _chord[0] = _targets[LANE_PITCH];`. When the chord has one note, `ChordBuilder::apply` returns `out_norm[0] = root_norm` for the root slot (`chord.h:142`), which is the same `_tg[LANE_PITCH]` value — so the write is a no-op either way. When it has more, the guard is false.

**Files:**
- Modify: `engine/parts/part.h` (add `_control_tick()` declaration and the `_tg` cache member)
- Modify: `engine/parts/part.cpp:96-162` (`Part::process`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `void Part::_control_tick()` (private, no args, no return) — Task 4 gates the call site. `float Part::_tg[LANE_COUNT]` (private) — the target cache Tasks 4 and 6 read and write.

- [ ] **Step 1: Add the members to `part.h`**

In `engine/parts/part.h`, inside the `private:` section, immediately after the `IPartEngine* _engine_for(EngineId e)` helper (around line 129), add:

```cpp
    // Rasterable half of process(): everything the engine consumes at its own
    // control tick. Task 1 calls it per sample; the raster arrives in Task 4.
    void _control_tick();

    // Target cache: _control_tick() fills it, process() pushes it to the
    // engine. Boot values mirror SynthEngine::_targets so a push before the
    // first tick cannot hand the engine garbage.
    float _tg[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };
```

- [ ] **Step 2: Rewrite `Part::process` and add `_control_tick` in `part.cpp`**

Replace the whole of `void Part::process(float& outL, float& outR, float& sendL, float& sendR)` (`engine/parts/part.cpp:96-162`) with:

```cpp
// Everything the engine reads at its own control tick: the five lane targets,
// the quantized pitch, the chord surface. Task 4 gates this behind a
// 96-sample raster; until then it runs per sample and the output is
// bit-identical to the pre-extraction code.
void Part::_control_tick() {
    for (int i = 0; i < LANE_COUNT; ++i) _tg[i] = target_raw(i);
    _tg[LANE_PITCH] = _quant.process(pitch_pre_quant());
    _pitch_q = _tg[LANE_PITCH];                              // clean, drives pitch_cv()
    _tg[LANE_PITCH] = clampf(_pitch_q + _detune_cents * (1.f / 3600.f), 0.f, 1.f);

    // chord layer: refresh the surface every tick (cheap interval apply);
    // full voice-leading build only on a fire
    // COLOR is MOTION's third destination, alongside pan fan and drift (spec
    // 2026-07-18 color-motion-target). Bipolar additive: the knob is the
    // centre, MOTION swings +/-kColorMod around it at MOD = 1. The gate makes
    // COLOR = 0 exactly silent by construction.
    const float cgate = clampf(_color / kColorGate, 0.f, 1.f);
    const float cmod  = _active[LANE_MOTION]
        ? _mod.lane_output(LANE_MOTION) * _depth * kColorMod * cgate
        : 0.f;
    _color_eff = clampf(_color + cmod, 0.f, 1.f);
    _chord.set_color(_color_eff);
    float chord[ChordBuilder::kMaxNotes];
    const int nch = _chord.apply(_tg[LANE_PITCH], _chord_mask(),
                                 _quant.root_semis(), chord);
    _engine->set_chord(chord, nch);
}

void Part::process(float& outL, float& outR, float& sendL, float& sendR) {
    _mod.process();

    // click-free engine switch: fade out (4 ms) -> swap -> fade in (4 ms).
    // At hold the multiplier is exactly 1.0, so unswitched runs stay
    // bit-identical (M1.6 bypass invariant).
    const float fade = _engine_fade.process();
    if (_switching && _engine_fade.is_idle()) {
        _engine_id = _pending_engine;
        _engine = _engine_for(_engine_id);
        _engine->set_flow(!_step_on);                          // re-sync state
        _engine->set_hold(_inhibit);
        if (_last_master_hz > 0.f) _engine->set_cycle(1.f / _last_master_hz);
        _switching = false;
        _engine_fade.set_on(true);
    }

    // forward the master-lane cycle length on change, not per sample
    const float hz = _mod.master_hz();
    if (hz != _last_master_hz && hz > 0.f) {
        _last_master_hz = hz;
        _engine->set_cycle(1.f / hz);
    }

    const bool fired = _mod.lane_fired(LANE_PITCH);
    if (fired) {
        _note_suppressed = _inhibit;
        if (!_inhibit) _gate_ctr = _gate_len;
    }
    if (_gate_ctr > 0) --_gate_ctr;

    _control_tick();

    _engine->set_targets(_tg, _tune);

    if (fired && !_note_suppressed) {
        float chord[ChordBuilder::kMaxNotes];
        const int nch = _chord.build(_tg[LANE_PITCH], _chord_mask(),
                                     _quant.root_semis(), chord);
        _engine->trigger_chord(chord, nch);
    }
    _engine->process(outL, outR);
    outL *= fade;
    outR *= fade;

    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
}
```

- [ ] **Step 3: Build and run the full test suite**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: configure + build succeed, `ctest` reports `100% tests passed, 0 tests failed out of 1`.

If any test fails, the extraction changed behaviour — the move was not faithful. Diff your `_control_tick()` against the original `part.cpp:126-154` line by line before changing anything else.

- [ ] **Step 4: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp
git commit -m "$(cat <<'EOF'
refactor(part): extract _control_tick(), still per sample

Pure move of the rasterable half of Part::process into its own method, so
the raster in the next commit can be judged on its own. set_targets moves
after the chord block; safe because SynthEngine::set_targets only touches
_chord[0] when the chord has one note, where apply() produced the same
value.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: The identity-gate scenario and its reference render

Task 4's correctness proof. The scenario is built to be structurally free of the two cadence-dependent hysteresis paths named in the spec, so its render **must** survive the raster byte-for-byte:

- `set_quant_mode: free` — `Quantizer::process` returns its input untouched with no state read (`engine/pitch/quantizer.h:57-62`), so `_last_note` hysteresis cannot diverge.
- COLOR stays at its boot 0.0, far below `ChordBuilder::kEdge2 = 0.125`, and the MOTION→COLOR path is silenced by `kColorGate` at COLOR 0 — so the zone hysteresis never moves.
- The PITCH lane runs at full depth so the rastered path is genuinely exercised, not idled.
- FX and reverb stay off: this gate is about the glue, and fewer moving parts means a diff points at one thing.

**Files:**
- Create: `host/render/scenarios/ctrl_identity.json`
- Produces, not committed: `renders/ctrl_identity.wav`, `renders/ctrl_identity.csv` -- `/renders/` is gitignored (15091b9); these are local render artifacts, not tracked reference files.

**Interfaces:**
- Consumes: `Part::_control_tick()` from Task 1 (indirectly — the render just has to run).
- Produces: the rendered WAV/CSV as a local sanity-check artifact. (No checksum gate: bit-exactness is not required for this pre-release engine -- see the branch-level note above -- and the `.sha256` this task originally tracked next to the scenario has since been removed as a stale, unconsumed file.)

- [ ] **Step 1: Write the scenario**

Create `host/render/scenarios/ctrl_identity.json`:

```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 8.0,
  "init": [
    { "action": "set_quant_mode", "part": 0, "value": "free" },
    { "action": "set_quant_mode", "part": 1, "value": "free" },
    { "action": "set_reverb_mix", "value": 0.0 },
    { "action": "set_depth", "part": 0, "value": 1.0 },
    { "action": "set_depth", "part": 1, "value": 1.0 },
    { "action": "set_rate", "part": 0, "value": 0.8 },
    { "action": "set_rate", "part": 1, "value": 0.55 },
    { "action": "set_target_depth", "part": 0, "slot": 2, "value": 1.0 },
    { "action": "set_target_depth", "part": 1, "slot": 2, "value": 1.0 },
    { "action": "set_smooth", "part": 0, "value": 0.0 },
    { "action": "set_smooth", "part": 1, "value": 0.0 }
  ],
  "events": []
}
```

`"slot": 2` is `LANE_PITCH` (`engine/mod/lane_id.h`; slot 0 is `LANE_SOURCE`). `set_smooth` at 0 is deliberate: it is the near-passthrough case the cut list flagged as the risk, so the gate covers it head-on.

- [ ] **Step 2: Verify the scenario loads and the field names are right**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake --build build --target render
./build/render host/render/scenarios/ctrl_identity.json renders/ctrl_identity.wav renders/ctrl_identity.csv
```

Expected: no `scenario error:` line, exit 0, both files written.

If it prints `scenario error: ...`, open `host/render/scenario.cpp` and check the exact key spelling for the offending action — the vocabulary there is authoritative over this plan.

- [ ] **Step 3: Confirm the render is not silent**

A gate that renders silence proves nothing.

```bash
python -c "
import wave, struct, sys
w = wave.open('renders/ctrl_identity.wav')
n = w.getnframes()
d = struct.unpack('<%dh' % (n * w.getnchannels()), w.readframes(n))
peak = max(abs(x) for x in d)
print('frames', n, 'peak', peak)
sys.exit(0 if peak > 1000 else 1)
"
```

Expected: `frames` around 384000, `peak` comfortably above 1000, exit 0.

If peak is ~0, the parts are not sounding — check that the scenario did not accidentally leave both parts inhibited, and that `duration_s` is being honoured.

- [ ] **Step 4: Record the reference hash**

```bash
sha256sum renders/ctrl_identity.wav | tee /tmp/ctrl_identity.sha256
```

Expected: one hash line. Keep this value — Tasks 4 and 5 compare against it. It is also recoverable at any time from git via the committed file.

- [ ] **Step 5: Commit**

```bash
git add host/render/scenarios/ctrl_identity.json renders/ctrl_identity.wav renders/ctrl_identity.csv
git commit -m "$(cat <<'EOF'
test(render): ctrl_identity scenario -- the control-rate identity gate

FREE quantizer, COLOR at 0, SMOOTH at 0, PITCH at full depth: structurally
free of both cadence-dependent hysteresis paths, so its render must survive
the raster byte-for-byte while still exercising the fast-lane risk case.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: `Quantizer` slew in caller-defined intervals

`Quantizer::init` sets `_slew_len = sample_rate * 0.04f` — a count of *calls*, which today equals samples. Called once per 96 samples it would stretch a 40 ms slew to 3.8 s. The parameter defaults to 1, so this task changes no behaviour anywhere; Task 4 is what passes 96.

**Files:**
- Modify: `engine/pitch/quantizer.h:41-46` (`init`)
- Test: `tests/test_quantizer.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `void Quantizer::init(float sample_rate, int call_interval = 1)` — Task 4 calls it as `_quant.init(sample_rate, SynthEngine::kCtrlInterval)`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_quantizer.cpp`:

```cpp
TEST_CASE("quantizer: slew length scales with the caller's interval") {
    // Called once per 96 samples, a 40 ms slew must still be 40 ms of audio
    // -- 1920 samples -- which is 20 calls, not 1920.
    //
    // Settling is detected by convergence, not by comparing against a fixed
    // value: the slewed output rises monotonically and then holds, so the
    // first post-change sample is a transient the output never revisits.
    // At root 0 and input 0.5 (= 18 semitones) the scale change moves the
    // nearest allowed note from 17 (dorian) to 18 (whole tone), so there is
    // a real distance to slew across.
    auto calls_to_settle = [](int interval) {
        Quantizer q;
        q.init(48000.f, interval);
        q.set_root(0);
        q.process(0.5f);                          // establish _last_out
        q.set_scale(SCALE_MASKS[SCALE_WHOLE]);    // on_change() arms the slew
        int calls = 0;
        float prev = q.process(0.5f);
        for (; calls < 5000; ++calls) {
            const float cur = q.process(0.5f);
            if (cur == prev) break;               // slew done, output holds
            prev = cur;
        }
        return calls;
    };

    CHECK(calls_to_settle(96) <= 25);    // ~20 control ticks, not ~1920
    CHECK(calls_to_settle(1)  > 100);    // default: still a sample-rate slew
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake --build build && ./build/spky_tests -ts="*" -tc="quantizer: slew length scales with the caller's interval"
```

Expected: a compile error — `init` takes one argument. That is the failure.

- [ ] **Step 3: Add the parameter**

In `engine/pitch/quantizer.h`, replace the `init` body (lines 41-46):

```cpp
    // call_interval = how many samples pass between process() calls. Part
    // drives the quantizer at the engine's control tick (96), so the 40 ms
    // change slew has to be counted in calls, not samples. Floored at 1 so a
    // large interval cannot collapse the slew to nothing.
    void init(float sample_rate, int call_interval = 1) {
        if (call_interval < 1) call_interval = 1;
        _slew_len = static_cast<int>(sample_rate * 0.04f
                                     / static_cast<float>(call_interval));
        if (_slew_len < 1) _slew_len = 1;
        _slew_ctr = 0;
        _have_note = false;
        _have_out = false;
    }
```

- [ ] **Step 4: Run the new test and the full suite**

```bash
cmake --build build && ./build/spky_tests -tc="quantizer: slew length scales with the caller's interval" && ctest --test-dir build --output-on-failure
```

Expected: the new test passes, and `ctest` still reports all tests passing. No existing caller passes a second argument, so nothing else may move.

- [ ] **Step 5: Commit**

```bash
git add engine/pitch/quantizer.h tests/test_quantizer.cpp
git commit -m "$(cat <<'EOF'
feat(quantizer): slew length in caller intervals, default unchanged

Part is about to drive the quantizer at the engine's 96-sample control
tick, where a sample-counted 40 ms slew would become 3.8 s. Defaults to 1,
so every existing caller is bit-identical.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 4: Engage the raster

Spec Step 1. `_control_tick()` goes behind a counter; LEVEL, the `set_targets` push and the FX targets stay per-sample.

**Files:**
- Modify: `engine/parts/part.h` (counter member)
- Modify: `engine/parts/part.cpp` (`init`, `process`)
- Test: `tests/test_part.cpp`

**Interfaces:**
- Consumes: `Part::_control_tick()` and `Part::_tg` (Task 1), `Quantizer::init(float, int)` (Task 3), the reference hash from Task 2.
- Produces: `int Part::_ctrl_ctr` (private). Task 6 reads no new interface; it edits `_control_tick` and `process` directly.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_part.cpp`:

```cpp
TEST_CASE("part: targets reach the engine on the 96-sample raster") {
    // The quantized pitch is recomputed at the control tick and held between
    // ticks. pitch_cv() reads _pitch_q, so it must be a staircase with 96-
    // sample treads -- not a fresh value every sample.
    Part p;
    p.init(48000.f, 3u);
    p.set_target_active(LANE_PITCH, true);
    p.set_target_base(LANE_PITCH, 0.5f);
    p.set_target_depth(LANE_PITCH, 1.f);
    p.set_depth(1.f);
    p.quant().set_mode(QuantMode::Free);    // no grid, so any change shows
    p.mod().set_range(1.f);
    p.mod().set_smooth(0.f);
    p.mod().set_rate(0.9f);

    float l, r;
    int changes = 0;
    float prev = p.pitch_cv();
    for (int i = 0; i < 96 * 20; ++i) {
        p.process(l, r);
        const float now = p.pitch_cv();
        if (now != prev) ++changes;
        prev = now;
    }
    // 20 intervals -> at most ~20 steps (plus any fire-driven refresh).
    // Per-sample evaluation would give hundreds.
    CHECK(changes > 0);
    CHECK(changes < 60);
}
```

- [ ] **Step 2: Run it and watch it fail**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake --build build && ./build/spky_tests -tc="part: targets reach the engine on the 96-sample raster"
```

Expected: FAIL — `changes` is in the hundreds, because `_control_tick()` still runs every sample.

- [ ] **Step 3: Add the counter to `part.h`**

In `engine/parts/part.h`, next to the `_control_tick()` declaration added in Task 1:

```cpp
    // Control raster. Phase-aligned with SynthEngine's own _ctrl_ctr by
    // construction: both init to 0, both advance once per Part::process, so
    // both fire on samples 0, 96, 192 -- and _control_tick() runs before
    // _engine->process() within that sample, which is the order the
    // per-sample code delivered. After a set_engine swap the engine's counter
    // is offset (it did not run while inactive), so it may read a target up
    // to one interval stale for one interval; the fade is at zero there.
    int _ctrl_ctr = 0;
```

- [ ] **Step 4: Engage it in `part.cpp`**

In `Part::init` (`engine/parts/part.cpp`), change the quantizer init line and reset the counter. Replace:

```cpp
    _quant.init(sample_rate);                   // boots Dorian / SCALE / root 0
```

with:

```cpp
    _ctrl_ctr = 0;                              // first process() runs a tick
    _quant.init(sample_rate, SynthEngine::kCtrlInterval);   // slew in ticks
```

Then in `Part::process`, replace the bare call:

```cpp
    _control_tick();
```

with:

```cpp
    // Raster, plus an event refresh: a PITCH fire samples the lane at that
    // exact sample, so a tick-stale pitch is not "late", it is the wrong
    // note. The refresh deliberately does not re-phase _ctrl_ctr -- the
    // alignment with the engine's own tick is the point.
    if (_ctrl_ctr == 0) {
        _ctrl_ctr = SynthEngine::kCtrlInterval;
        _control_tick();
    } else if (fired) {
        _control_tick();
    }
    --_ctrl_ctr;

    _tg[LANE_LEVEL] = target_raw(LANE_LEVEL);   // per-sample engine consumer
```

Leave the existing `_engine->set_targets(_tg, _tune);` line where it is, directly below.

- [ ] **Step 5: Run the new test and the full suite**

```bash
cmake --build build && ./build/spky_tests -tc="part: targets reach the engine on the 96-sample raster" && ctest --test-dir build --output-on-failure
```

Expected: the new test passes and all existing tests pass.

Existing tests should survive: `target_value(slot)` computes live from `target_raw` for non-PITCH slots (`part.cpp:54`) and is not served from `_tg`, so assertions on it are unaffected. If a test that rides the quantizer slew fails, check that it allows for the slew now being counted in ticks — report it rather than loosening the assertion on your own judgement.

- [ ] **Step 6: The identity gate**

**Bit-exactness is not required** (Bastian, 2026-07-19): the engine is pre-release and other CPU work is landing in parallel, so no checksum gates this. Render the scenario to confirm the raster did not break the audio outright, and report what you observe.

```bash
cmake --build build --target render
echo "RENDER_BUILD_EXIT=$?"
./build/render host/render/scenarios/ctrl_identity.json /tmp/ctrl_identity_after.wav /tmp/ctrl_identity_after.csv
echo "RENDER_EXIT=$?"
python -c "
import wave, struct
w = wave.open('/tmp/ctrl_identity_after.wav'); n = w.getnframes()
d = struct.unpack('<%dh' % (n * w.getnchannels()), w.readframes(n))
print('frames', n, 'peak', max(abs(x) for x in d))
"
```

Expected: both exit codes 0, 384000 frames, a peak in the same order as Task 2's 12590 — audio, not silence and not clipping.

Check the exit codes explicitly as shown. Do not pipe the build through `head`/`tail` — the pipeline's status masks a failed build and the next command then runs against a stale binary.

- [ ] **Step 7: Render the default scenario too**

```bash
./build/render host/render/scenarios/chord_bloom.json /tmp/chord_bloom_after.wav /tmp/chord_bloom_after.csv
echo "RENDER_EXIT=$?"
```

Expected: exit 0. This scenario runs the scale quantizer and a moving COLOR — both cadence-dependent hysteresis paths — so its audio is expected to shift slightly under the raster. Confirm it still renders and is not silent; do not commit anything under `renders/`.

- [ ] **Step 8: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "$(cat <<'EOF'
perf(part): glue targets and chord surface on the 96-sample raster

The engine already read them once per block; Part computed them 96 times.
Counter lives in Part, phase-aligned with SynthEngine's by construction. A
PITCH fire refreshes off-raster, because a tick-stale pitch under SMOOTH=0
is the wrong note, not a late one. LEVEL, the set_targets push and the FX
targets stay per-sample for now.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 5: Instrument-level identity check

Task 4 proved a standalone-ish path through the render CLI. This confirms nothing drifted at the `Instrument` seam, where `Center::update` writes `set_detune_cents` on its own 96-raster and both parts interleave under CHOKE.

**Files:**
- Test: `tests/test_instrument.cpp`

**Interfaces:**
- Consumes: everything from Task 4.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Write the test**

Append to `tests/test_instrument.cpp`:

```cpp
TEST_CASE("instrument: control raster survives a block-size-agnostic call pattern") {
    // The raster lives in Part and advances per sample, so rendering the same
    // audio in 96-sample blocks and in 7-sample blocks must give the same
    // samples. If anything ever ties the tick to the host block boundary,
    // this is what catches it.
    auto render = [](size_t chunk, std::vector<float>& out) {
        Instrument inst;
        inst.init(48000.f);
        inst.set_tempo_bpm(120.f);
        for (int p = 0; p < PART_COUNT; ++p) {
            inst.set_depth(p, 1.f);
            inst.set_rate(p, 0.8f);
        }
        out.assign(4800, 0.f);
        std::vector<float> r(4800, 0.f);
        for (size_t i = 0; i < 4800; i += chunk) {
            const size_t n = std::min(chunk, size_t(4800) - i);
            inst.process(nullptr, nullptr, out.data() + i, r.data() + i, n);
        }
    };
    std::vector<float> a, b;
    render(96, a);
    render(7, b);
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}
```

If `test_instrument.cpp` does not already include `<vector>` and `<algorithm>`, add them at the top.

- [ ] **Step 2: Run it**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake --build build && ./build/spky_tests -tc="instrument: control raster survives a block-size-agnostic call pattern"
```

Expected: PASS. It should pass on the first try — it is a guard, not a driver. If it fails, the raster picked up a dependency on the host block size and that is a real bug in Task 4's counter; report it.

- [ ] **Step 3: Commit**

```bash
git add tests/test_instrument.cpp
git commit -m "$(cat <<'EOF'
test(instrument): control raster is independent of host block size

Guard for the Part-owned tick: same audio whether the host calls in 96- or
7-sample chunks.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 6: The remaining per-sample consumers

Spec Step 2. LEVEL, the five FX targets and the `set_targets` push move onto the tick. **This is the task where the render legitimately changes.** `PartFx`'s 2 ms smoothers (`part_fx.cpp:27`) and the engine's 10 ms `_level` absorb the steps.

**Files:**
- Modify: `engine/parts/part.h` (`_fxv` cache member)
- Modify: `engine/parts/part.cpp` (`_control_tick`, `process`)
- Re-cut, not committed: the `ctrl_identity` render, as a local sanity check -- `/renders/` is gitignored, nothing under it is tracked.

**Interfaces:**
- Consumes: Task 4's raster.
- Produces: `float Part::_fxv[FXT_COUNT]` (private).

- [ ] **Step 1: Add the FX cache to `part.h`**

Next to the `_tg` member from Task 1:

```cpp
    // FX target cache, filled at the control tick. PartFx smooths each value
    // over 2 ms, so the raster's steps never reach an FX parameter raw.
    // Boot values mirror _fx_base so the first block cannot push zeros.
    float _fxv[FXT_COUNT] = { 0.3f, 0.4f, 1.f, 0.25f, 0.45f };
```

- [ ] **Step 2: Move the work into `_control_tick`**

At the end of `Part::_control_tick()`, after the `_engine->set_chord(chord, nch);` line, add:

```cpp
    _engine->set_targets(_tg, _tune);
    for (int i = 0; i < FXT_COUNT; ++i) _fxv[i] = fx_target_value(i);
```

- [ ] **Step 3: Strip the per-sample copies from `process`**

Delete these two lines from `Part::process` (added in Task 4 / carried from Task 1):

```cpp
    _tg[LANE_LEVEL] = target_raw(LANE_LEVEL);   // per-sample engine consumer

    _engine->set_targets(_tg, _tune);
```

and replace the FX block at the end of `process`:

```cpp
    float fxv[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) fxv[i] = fx_target_value(i);
    _fx.process(outL, outR, sendL, sendR, fxv);
```

with:

```cpp
    _fx.process(outL, outR, sendL, sendR, _fxv);
```

- [ ] **Step 4: Run the full suite**

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
source env.sh
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: all tests pass. A test asserting an exact LEVEL-driven amplitude within the first 96 samples could legitimately move now; if one fails, report which and why before touching it.

- [ ] **Step 5: Render and sanity-check the audio**

Bit-exactness is not required (Bastian, 2026-07-19) — this task changes the render on purpose. What matters is that the change is a smoothed step and not a click or a dropout.

```bash
cmake --build build --target render
echo "RENDER_BUILD_EXIT=$?"
./build/render host/render/scenarios/ctrl_identity.json /tmp/ctrl_after6.wav /tmp/ctrl_after6.csv
echo "RENDER_EXIT=$?"
python -c "
import wave, struct
w = wave.open('/tmp/ctrl_after6.wav'); n = w.getnframes()
d = struct.unpack('<%dh' % (n * w.getnchannels()), w.readframes(n))
peak = max(abs(x) for x in d)
jump = max(abs(d[i+2] - d[i]) for i in range(0, len(d) - 2, 2))
print('frames', n, 'peak', peak, 'max sample-to-sample jump', jump)
"
```

Expected: both exit codes 0, 384000 frames, peak in the same order as Task 2's 12590. Report the max jump — a large jump against a modest peak is the signature of a step reaching the output unsmoothed, which is what the FX and level smoothers are supposed to prevent. Investigate before committing if it looks that way.

Do not pipe the build through `head`/`tail`: the pipeline's exit status hides a failed build and the render then runs a stale binary.

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp
git commit -m "$(cat <<'EOF'
perf(part): LEVEL, the target push and the FX targets onto the raster

The last per-sample glue consumers. PartFx's 2 ms smoothers and the
engine's 10 ms level smoother absorb the steps. This changes the render on
purpose, so the identity gate's checksum moves with it.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 7: Measure on hardware — human step

The saving is not a claim this plan may make on its own. `bench/run.py` needs a Daisy Seed attached over ST-Link, so an agent cannot self-serve it.

**Files:**
- Create: `docs/bench/YYYY-MM-DD-<hash>.md`, `docs/bench/YYYY-MM-DD-<hash>.csv` (written by the runner)

- [ ] **Step 1: Hand off to Bastian**

Report that the branch is ready to measure and that this step needs the hardware. The command, run from a clean tree so the result files get the right git hash:

```bash
cd "/c/Users/bernd/Documents/AI/Spotykach"
git status --short          # must be empty
python bench/run.py
```

- [ ] **Step 2: Read the result against the prediction**

The rows that matter:

| Row | Before (`9be5df9`) | What to check |
|---|---:|---|
| `part_glue_flow` | 287 313 avg | the cut itself |
| `instrument_worst` | 1 448 033 avg (150.83 %) | the budget effect |
| anchored `instrument_worst` | 152.03 % avg | the figure that decides |

The spec predicted 70–85 % of the glue, ≈16–19 % of budget across both parts, and named the assumption it can fail on: that `Quantizer::process` dominates. If the measured cut is materially below that, the distribution was flatter than assumed — record it as the finding rather than reaching for the next cut immediately.

Note that `part_glue_flow` subtracts `super_mod_5lanes`, an engine intercept and `fx_none` to isolate the glue (`bench/workloads_abl.cpp:64`); the raw row includes those, so compare like with like.

- [ ] **Step 3: Write the outcome up**

Append `## Outcome (YYYY-MM-DD)` to this plan file with the measured numbers, then update `docs/roadmap.md`: the bench-report reference, the `instrument_worst` figures, and the ranked cut list — item 1 is now spent, and the remaining items' predictions should be restated against the new budget rather than the old one.

```bash
git add docs/bench/ docs/roadmap.md docs/superpowers/plans/2026-07-19-part-glue-control-rate.md
git commit -m "$(cat <<'EOF'
docs: the part-glue cut, measured

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Self-review notes

Spec coverage checked section by section: raster ownership → Task 4 Step 3; the three paths → Tasks 1, 4, 6; quantizer slew → Task 3; Step 1 identity gate with its named condition → Task 2 + Task 4 Step 6; Step 2 → Task 6; expected saving and the assumption it fails on → Task 7 Step 2; verification table → Tasks 4, 5, 6, 7.

Two spec items are deliberately *not* tasks. The listening pass after Task 6 is Bastian's, not an agent's. The `set_engine`-swap counter offset is documented in the comment added in Task 4 Step 3 and left unfixed, as the spec says.
