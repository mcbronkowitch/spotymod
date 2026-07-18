# COLOR as a MOTION target Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make chord density vary per note by letting the MOTION lane modulate COLOR additively, so each stab in a phrase carries a different number of tones.

**Architecture:** COLOR stops being written straight into the `ChordBuilder`. `Part` stores the knob value in a new `_color` member and, once per sample in `process()`, computes an effective color — the knob plus a bipolar swing from `MOTION`'s lane output, scaled by the master MOD macro and a zero-gate that fades the swing in over the first 1% of knob travel — then pushes it via `_chord.set_color()` immediately before the existing `_chord.apply()` call. No new lane slot, no new panel control, no parameter id.

**Tech Stack:** C++17, doctest (`tests/`), CMake + Ninja + clang (`env.sh`), desktop render host (`host/render/`) for scenario WAV/CSV renders.

**Spec:** `docs/superpowers/specs/2026-07-18-color-motion-target-design.md`

## Global Constraints

- Repository: `c:\Users\bernd\Documents\AI\Spotykach` (the `spotymod` fork). Branch: `main`. Work directly on `main`.
- Build/test from the repo root:
  ```bash
  source env.sh
  cmake -S . -B build
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```
- **Bit-identity invariant (non-negotiable):** with `COLOR = 0` the chord layer must emit exactly one note whose value is the bit-exact root passthrough — no `Approx`, `==` on floats. This must hold for *any* MOD value and *any* MOTION state. Existing tests in `tests/test_chord.cpp` and `tests/test_part.cpp` encode it; none of them may be weakened.
- **Today's-behaviour invariant:** with `MOD = 0` (`Part::set_depth(0)`) the value reaching the `ChordBuilder` equals the COLOR knob exactly.
- Constants live in `engine/parts/part.h` as `static constexpr float` next to `kLevelFloor`, in the same commented style, and are labelled ear-tunable.
- `kColorMod = 0.2f`, `kColorGate = 0.01f`. Exact values, do not round or "improve" them.
- No change to `engine/pitch/chord.h` (zones, hysteresis, voice-leading), to pan/drift, to any parameter id, or to the VCV panel.
- Commit messages follow the repo's existing style (`feat(color): …`, `test(color): …`, `chore(renders): …`). Every commit trailer:
  ```
  Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
  ```
- Do not add a `Claude`/`Anthropic` co-author trailer.

## Deviation from the spec (deliberate, already decided)

The spec's testing section proposes `COLOR = 0.005, MOD = 1` as the "barely-open knob reaches upward into chords" case. That value cannot pass, arithmetically: at `COLOR = 0.005` the gate is `0.005 / 0.01 = 0.5`, so the swing is `±0.2 × 0.5 = ±0.1` and the peak reaches `0.105` — below the 2-note zone edge at `0.125 + kHyst (0.02) = 0.145`. The plan uses **`COLOR = 0.02`** instead: the gate is fully open, the peak reaches `0.22`, and 2% of knob travel is still unambiguously "barely open". The spec's intent is preserved; only the probe value changes.

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `engine/parts/part.h` | Owns the COLOR knob value and the two tuning constants; `set_color` becomes a plain store | Modify |
| `engine/parts/part.cpp` | Computes `color_eff` per sample in `process()` and pushes it into the `ChordBuilder` | Modify |
| `tests/test_part.cpp` | All new behavioural tests (invariants, spread, gate, determinism) | Modify |
| `host/render/scenarios/chord_bloom.json` | Comment update — it now demonstrates breathing density | Modify |
| `renders/chord_bloom.wav`, `renders/chord_bloom.csv` | Re-cut reference render | Modify (regenerate) |
| `docs/roadmap.md` | Record the landed feature | Modify |

`engine/instrument.h` needs **no** change: `set_color(int p, float n)` already forwards to `Part::set_color`, whose signature is unchanged.

---

### Task 1: COLOR becomes Part state, pushed per sample (no behaviour change)

Pure refactor. The knob value moves from the `ChordBuilder` into `Part`, and `process()` pushes it back every sample. Observable behaviour is identical; this task exists so that Task 2's modulation has somewhere to hook in, and so a reviewer can confirm the refactor alone changed nothing.

**Files:**
- Modify: `engine/parts/part.h` (the `set_color` inline, ~line 33; the private data block near `kLevelFloor`, ~line 130)
- Modify: `engine/parts/part.cpp` (`Part::process`, the chord-layer block, ~lines 134-145)
- Test: `tests/test_part.cpp` (append at end of file)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `Part::_color` (private `float`, the raw knob value, clamped to `[0,1]`). `Part::set_color(float)` keeps its signature `void set_color(float c)` and its clamping, but only stores. Task 2 reads `_color` and adds a `Part::color_eff()` accessor.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_part.cpp`:

```cpp
TEST_CASE("part: COLOR reaches the chord builder through process(), not the setter") {
    Part p;
    p.init(48000.f, 5u);
    p.set_depth(0.f);                 // MOD = 0: today's-behaviour invariant
    p.set_color(0.5f);
    float l, r;
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 3);       // triad zone, exactly the knob position
    p.set_color(0.75f);
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 4);
    p.set_color(0.f);
    for (int i = 0; i < 4800; ++i) p.process(l, r);
    CHECK(p.chord_size() == 1);
}
```

- [ ] **Step 2: Run the test to verify it passes on the current code**

```bash
source env.sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure -R part
```

Expected: PASS. This test is a *characterization* test — it pins the behaviour that must survive the refactor. If it fails now, stop and report; the premise is wrong.

- [ ] **Step 3: Move the knob value into `Part`**

In `engine/parts/part.h`, replace the `set_color` inline:

```cpp
    // COLOR (spec 2026-07-17 chord-layer): 0 = single note (bit-identical),
    // sweeps to a 4-note diatonic chord. Live on the FLOW surface.
    void set_color(float c) { _chord.set_color(clampf(c, 0.f, 1.f)); }
```

with:

```cpp
    // COLOR (spec 2026-07-17 chord-layer): 0 = single note (bit-identical),
    // sweeps to a 4-note diatonic chord. Live on the FLOW surface.
    // The knob is stored, not pushed: process() combines it with the MOTION
    // lane and hands the ChordBuilder the effective color (spec 2026-07-18
    // color-motion-target).
    void set_color(float c) { _color = clampf(c, 0.f, 1.f); }
```

In the same file, add the member immediately after the `_detune_cents` declaration in the private block:

```cpp
    float _detune_cents = 0.f;   // DRIFT detune, applied post-quantizer to the engine only
    float _color = 0.f;          // COLOR knob; effective color is computed in process()
```

- [ ] **Step 4: Push it from `process()`**

In `engine/parts/part.cpp`, in `Part::process`, replace:

```cpp
    // chord layer: refresh the surface every sample (cheap interval apply);
    // full voice-leading build only on a fire
    float chord[ChordBuilder::kMaxNotes];
    int nch = _chord.apply(targets[LANE_PITCH], _chord_mask(),
```

with:

```cpp
    // chord layer: refresh the surface every sample (cheap interval apply);
    // full voice-leading build only on a fire
    _chord.set_color(_color);
    float chord[ChordBuilder::kMaxNotes];
    int nch = _chord.apply(targets[LANE_PITCH], _chord_mask(),
```

- [ ] **Step 5: Run the full suite**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, all tests, zero failures. Pay attention to `test_part.cpp`'s existing `part: chord size follows COLOR`, `part: COLOR never touches pitch CV or gate`, `part: COLOR swept and returned to 0 renders like never touched`, and everything in `test_chord.cpp` — all must still pass untouched.

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "$(cat <<'EOF'
refactor(color): Part owns the COLOR knob, process() pushes it

Groundwork for COLOR as a MOTION target: the knob value moves out of the
ChordBuilder into Part and is pushed once per sample, on the same cadence
apply() already runs at. No behaviour change.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 2: MOTION modulates COLOR — bipolar additive with a zero-gate

The feature. MOTION's bipolar lane output, scaled by the master MOD macro and gated by knob position, is added to the knob value.

**Files:**
- Modify: `engine/parts/part.h` (constants next to `kLevelFloor`; a `color_eff()` accessor in the public block near `chord_size()`)
- Modify: `engine/parts/part.cpp` (`Part::process`, the line added in Task 1)
- Test: `tests/test_part.cpp` (append at end of file)

**Interfaces:**
- Consumes: `Part::_color` from Task 1; the existing `Part::_depth` (master MOD, set by `set_depth`), `_active[LANE_MOTION]`, and `_mod.lane_output(LANE_MOTION)` (bipolar −1…+1 — texture lanes sit at full range; `set_range` touches `LANE_PITCH` only).
- Produces: `float Part::color_eff() const` — the effective color last pushed to the `ChordBuilder`, for tests and future hosts. `static constexpr float Part::kColorMod = 0.2f` and `Part::kColorGate = 0.01f` (private).

Note: the per-target depth `_tdepth[LANE_MOTION]` is deliberately **not** in the formula — it governs pan and drift. Only the master MOD scales the COLOR swing. Do not "fix" this.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_part.cpp`:

```cpp
// --- COLOR as a MOTION target (spec 2026-07-18 color-motion-target) ---

namespace {
// Run one part for `n` samples, recording the chord size on every PITCH-lane
// fire. Returns the sizes seen, in order.
static std::vector<int> chord_sizes_over(Part& p, int n) {
    std::vector<int> sizes;
    float l, r;
    for (int i = 0; i < n; ++i) {
        p.process(l, r);
        if (p.lane_fired(LANE_PITCH)) sizes.push_back(p.chord_size());
    }
    return sizes;
}
} // namespace

TEST_CASE("color-mod: COLOR 0 stays one note whatever MOD and MOTION do") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);      // 10 s, many MOTION cycles
    REQUIRE(!sizes.empty());
    for (int n : sizes) CHECK(n == 1);
}

TEST_CASE("color-mod: MOD 0 hands the ChordBuilder the knob, exactly") {
    Part p;
    p.init(48000.f, 5u);
    p.set_depth(0.f);
    p.set_target_active(LANE_MOTION, true);
    float l, r;
    for (float knob : {0.f, 0.2f, 0.5f, 0.77f, 1.f}) {
        p.set_color(knob);
        for (int i = 0; i < 480; ++i) p.process(l, r);
        CHECK(p.color_eff() == knob);              // exact, not Approx
    }
}

TEST_CASE("color-mod: a barely-open knob reaches up into chords") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.02f);                            // 2% of travel; gate fully open
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);
    REQUIRE(!sizes.empty());
    int maxn = 0;
    for (int n : sizes) if (n > maxn) maxn = n;
    CHECK(maxn >= 2);                              // the swing is additive, not a ceiling
}

TEST_CASE("color-mod: density varies per note at a mid knob position") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.35f);                            // near the 2/3-note zone edge (0.375)
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);
    p.set_step(true, 8);
    auto sizes = chord_sizes_over(p, 480000);
    REQUIRE(sizes.size() > 4);
    int mn = sizes[0], mx = sizes[0];
    for (int n : sizes) { if (n < mn) mn = n; if (n > mx) mx = n; }
    CHECK(mn < mx);                                // spread, not specific sizes
}

TEST_CASE("color-mod: an inactive MOTION target modulates nothing") {
    Part p;
    p.init(48000.f, 5u);
    p.set_color(0.5f);
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, false);
    float l, r;
    for (int i = 0; i < 48000; ++i) {
        p.process(l, r);
        CHECK(p.color_eff() == 0.5f);              // exact
    }
}

TEST_CASE("color-mod: deterministic — same seed, same density sequence") {
    Part a, b;
    a.init(48000.f, 7u);
    b.init(48000.f, 7u);
    for (Part* p : {&a, &b}) {
        p->set_color(0.35f);
        p->set_depth(1.f);
        p->set_target_active(LANE_MOTION, true);
        p->set_step(true, 8);
    }
    auto sa = chord_sizes_over(a, 240000);
    auto sb = chord_sizes_over(b, 240000);
    CHECK(sa == sb);
    REQUIRE(!sa.empty());
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build 2>&1 | tail -20
```

Expected: FAIL to compile, with an error naming `color_eff` — `no member named 'color_eff' in 'spky::Part'`. That is the correct failure; do not proceed until you see it.

- [ ] **Step 3: Add the constants and the accessor**

In `engine/parts/part.h`, add to the public block immediately after `int chord_size() const { return _chord.size(); }`:

```cpp
    // The color actually handed to the ChordBuilder: the knob plus MOTION's
    // swing (spec 2026-07-18 color-motion-target). Equals the knob when
    // MOD = 0 or the MOTION target is inactive.
    float color_eff() const { return _color_eff; }
```

In the private block, immediately after the `kLevelFloor` constant and its comment, add:

```cpp
    // COLOR is a third destination of the MOTION lane (spec 2026-07-18
    // color-motion-target): density pendles +/-1 zone around the knob, so a
    // phrase's stabs differ in size. Bipolar and ADDITIVE, so the reach stays
    // constant across the knob range and a barely-open knob can still rise
    // into chord territory. Both ear-tunable.
    //   kColorMod  swing amplitude at MOD = 1; the zones are 0.25 wide, so
    //              +/-0.2 crosses at most one edge in each direction.
    //   kColorGate knob travel over which the swing fades in. Below it the
    //              swing is scaled toward 0, so COLOR = 0 is structurally
    //              silent (multiplied by zero, not special-cased) and the
    //              chord layer's bit-identity guarantee survives untouched.
    static constexpr float kColorMod  = 0.2f;
    static constexpr float kColorGate = 0.01f;
```

And add the cached value next to `_color`:

```cpp
    float _color = 0.f;          // COLOR knob; effective color is computed in process()
    float _color_eff = 0.f;      // knob + MOTION swing, as last pushed to _chord
```

- [ ] **Step 4: Compute it in `process()`**

In `engine/parts/part.cpp`, replace the single line added in Task 1:

```cpp
    _chord.set_color(_color);
```

with:

```cpp
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
```

- [ ] **Step 5: Run the full suite**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: PASS, all tests. In particular the six new `color-mod:` cases, plus every pre-existing `test_chord.cpp` and `test_part.cpp` case unchanged.

If `color-mod: density varies per note at a mid knob position` fails with `mn == mx`, do **not** raise `kColorMod`. Move the probe knob closer to a zone edge (the edges are 0.125 / 0.375 / 0.625, hysteresis `kHyst = 0.02`) and report which position you used.

- [ ] **Step 6: Commit**

```bash
git add engine/parts/part.h engine/parts/part.cpp tests/test_part.cpp
git commit -m "$(cat <<'EOF'
feat(color): MOTION modulates COLOR — density varies per note

COLOR becomes a third destination of the MOTION lane, alongside pan fan and
drift. Bipolar additive with a zero-gate: the knob is the centre, MOTION
swings +/-0.2 around it at MOD = 1, and the gate fades the swing in over the
first 1% of travel so COLOR = 0 stays structurally single-note.

Two invariants hold by construction, not by tuning: COLOR = 0 is always one
note (bit-identity preserved), and MOD = 0 is exactly today's behaviour.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

### Task 3: Re-cut the renders and record the landing

The spec calls this out as a certainty, not a risk: `chord_bloom` sweeps COLOR to 0.95 and never calls `set_depth`, so it runs at the boot `_depth = 1.0` with MOTION active. Its chords will now breathe. The other chord-layer scenarios sit at COLOR 0 and must stay byte-identical — that is the gate.

**Files:**
- Modify: `host/render/scenarios/chord_bloom.json` (the `_comment` in `init`, line 6)
- Modify: `renders/chord_bloom.wav`, `renders/chord_bloom.csv` (regenerate)
- Modify: `docs/roadmap.md`

**Interfaces:**
- Consumes: the landed behaviour from Task 2. Nothing produced for later tasks.

- [ ] **Step 1: Capture the pre-change hashes of the COLOR-0 scenarios**

The three chord-layer baselines that sit at COLOR 0 must not move. Render them from the **pre-change** build first:

```bash
git stash list   # expect empty; you are on a clean tree at Task 2's commit
git checkout HEAD~2 -- engine/parts/part.h engine/parts/part.cpp
cmake --build build
for s in ambient_wash demo_step_melody demo_density_sweep; do
  ./build/render.exe host/render/scenarios/$s.json /tmp/pre_$s.wav /tmp/pre_$s.csv
done
sha256sum /tmp/pre_*.wav
git checkout HEAD -- engine/parts/part.h engine/parts/part.cpp
cmake --build build
```

Record the three hashes in your report.

- [ ] **Step 2: Render the same three from the post-change build and compare**

```bash
for s in ambient_wash demo_step_melody demo_density_sweep; do
  ./build/render.exe host/render/scenarios/$s.json /tmp/post_$s.wav /tmp/post_$s.csv
done
sha256sum /tmp/post_*.wav
for s in ambient_wash demo_step_melody demo_density_sweep; do
  cmp /tmp/pre_$s.wav /tmp/post_$s.wav && echo "$s: IDENTICAL"
done
```

Expected: all three print `IDENTICAL` and the hash pairs match. **If any differ, stop and report — that is a bit-identity regression, not something to re-baseline.**

- [ ] **Step 3: Update the `chord_bloom` comment**

In `host/render/scenarios/chord_bloom.json`, replace the `_comment` value on line 6:

```json
  { "_comment": "COLOR showcase. Part A: Dorian FLOW drone that blooms into a pad (live COLOR sweep 8-20s), collapses (24-28s), then STEP chord stabs at COLOR 0.6 (32s on). Part B silenced (boot lanes would siren)." },
```

with:

```json
  { "_comment": "COLOR showcase. Part A: Dorian FLOW drone that blooms into a pad (live COLOR sweep 8-20s), collapses (24-28s), then STEP chord stabs at COLOR 0.6 (32s on). Density also breathes per note: MOD sits at the boot 1.0 with MOTION active, so MOTION swings COLOR +/-0.2 around the knob (spec 2026-07-18 color-motion-target) and the stabs from 32s differ in size. Part B silenced (boot lanes would siren)." },
```

- [ ] **Step 4: Re-cut the `chord_bloom` reference render**

```bash
./build/render.exe host/render/scenarios/chord_bloom.json renders/chord_bloom.wav renders/chord_bloom.csv
ls -l renders/chord_bloom.wav renders/chord_bloom.csv
```

Expected: both files rewritten, non-zero size, `chord_bloom.wav` around 48 s of 16-bit stereo at 48 kHz (~9 MB).

Then confirm the render actually breathes — the STEP stabs from 32 s on should not all carry the same density:

```bash
git diff --stat renders/chord_bloom.csv
```

Expected: a non-empty diff. If the CSV is byte-identical to the old one, the feature is not reaching the render — stop and report.

- [ ] **Step 5: Record it in the roadmap**

In `docs/roadmap.md`, find the chord-layer / COLOR section and add an entry in the surrounding style, matching how neighbouring landed features are written. It must state: COLOR is a third destination of the MOTION lane; bipolar additive with a zero-gate; density varies per note in STEP and blooms/collapses in FLOW; no new panel control or parameter id; `COLOR = 0` and `MOD = 0` are unchanged by construction. Read 40 lines of surrounding context first and match the prose voice and heading depth — do not invent a new format.

- [ ] **Step 6: Run the full suite once more and commit**

```bash
ctest --test-dir build --output-on-failure
```

Expected: PASS, all tests.

```bash
git add host/render/scenarios/chord_bloom.json renders/chord_bloom.wav renders/chord_bloom.csv docs/roadmap.md
git commit -m "$(cat <<'EOF'
chore(renders): re-cut chord_bloom, COLOR-0 baselines proven identical

chord_bloom runs at the boot MOD 1.0 with MOTION active, so its chords now
breathe instead of holding a flat density — reference re-cut. The three
COLOR-0 chord-layer scenarios render byte-identical, as designed.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>
EOF
)"
```

---

## Out of scope

- No new depth control for the COLOR destination.
- No selectable sender — MOTION is wired in.
- No change to pan/drift, to the `ChordBuilder`'s zone or voice-leading logic, or to any parameter id.
- No VCV panel or hardware mapping change.
- No listening pass — that is Bastian's, after landing.
