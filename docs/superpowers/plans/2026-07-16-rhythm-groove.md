# Ranked-Slot Groove Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the metric-weight gate threshold with a composed, seeded groove ranking so the melody engine produces syncopated, repeating rhythm patterns, with DENSE as a monotonic "how many notes of the groove" knob and composed note durations on the GATE output.

**Architecture:** A new `GrooveCell` (rank permutation + note lengths, period = motif length L) is generated once per phrase in `phrase_gen.h` and stored in `ModLane`. `_effective_gate` becomes a rank-vs-k lookup (k from DENSE); `pg_gen_motif` gates become all-true; `gate_state()` gains note-sustain tracking. Spec: `docs/superpowers/specs/2026-07-16-rhythm-groove-design.md`.

**Tech Stack:** C++17 header-mostly engine, doctest, CMake+Ninja+clang (desktop tests).

## Global Constraints

- Work directly on `main` (user's instruction; no worktree).
- Engine code: no heap, no exceptions, no `Date`/time/global RNG — every draw through the caller's `spky::Rng`, in a fixed documented order (bit-determinism invariant).
- `phrase_gen.h` stays header-only; buffers are fixed 32-slot arrays.
- Build/test (Bash tool, POSIX): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build` then run `./build/spky_tests.exe`. Doctest filter: `./build/spky_tests.exe -tc="<pattern>"` (wildcards allowed).
- Every commit message ends with: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` (never the default Claude trailer).
- Tuning constants (sync degree range, push bonus, demotion, jitter, length distribution) are code-level, tuned by ear later; the tests below assert behavior, not constants — keep them robust to constant tweaks within the documented ranges.

---

### Task 1: `GrooveCell` + `pg_gen_groove` (phrase_gen)

**Files:**
- Modify: `engine/mod/phrase_gen.h` (append before the closing `} // namespace spky`)
- Test: `tests/test_phrase_gen.cpp` (append)

**Interfaces:**
- Consumes: `spky::Rng` (`next_unipolar()`), `pg_metric_weight(int)`.
- Produces: `struct GrooveCell { uint8_t rank_of_slot[32]; uint8_t note_len[32]; uint8_t len; }` and `void pg_gen_groove(Rng& rng, int L, GrooveCell& out)`. Task 3 stores a `GrooveCell` in `ModLane` and reads `rank_of_slot`/`len`; Task 4 reads `note_len`.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_phrase_gen.cpp`:

```cpp
TEST_CASE("groove: deterministic per seed, valid permutation, anchor rank 0") {
    for (uint32_t seed : {1u, 0xBEEFu, 0xC0FFEEu}) {
        Rng a; a.seed(seed);
        Rng b; b.seed(seed);
        GrooveCell ga, gb;
        pg_gen_groove(a, 8, ga);
        pg_gen_groove(b, 8, gb);
        bool seen[8] = {};
        for (int i = 0; i < 8; ++i) {
            CHECK(ga.rank_of_slot[i] == gb.rank_of_slot[i]);   // determinism
            CHECK(ga.note_len[i] == gb.note_len[i]);
            REQUIRE(ga.rank_of_slot[i] < 8);
            seen[ga.rank_of_slot[i]] = true;
        }
        for (int i = 0; i < 8; ++i) CHECK(seen[i]);            // permutation
        CHECK(ga.rank_of_slot[0] == 0);                        // anchor is always first
        CHECK(ga.len == 8);
    }
}

TEST_CASE("groove: note lengths in [1,4], biased short") {
    float sum = 0.f; int count = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        Rng r; r.seed(seed * 2654435761u);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        for (int i = 0; i < 8; ++i) {
            REQUIRE(g.note_len[i] >= 1);
            REQUIRE(g.note_len[i] <= 4);
            sum += g.note_len[i]; ++count;
        }
    }
    CHECK(sum / count < 2.2f);   // mean ~1.7: short notes common, sustains rare
}

TEST_CASE("groove: syncopation occurs — off-beats reach the top ranks") {
    int synced = 0;
    const int kSeeds = 400;
    for (uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng r; r.seed(seed * 0x9E3779B9u);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        for (int i = 1; i < 8; i += 2)                 // any odd slot in the top half?
            if (g.rank_of_slot[i] < 4) { ++synced; break; }
    }
    CHECK(synced > kSeeds / 4);   // pushes are a real, common feature
}

TEST_CASE("groove: L=1 degenerates cleanly") {
    Rng r; r.seed(3);
    GrooveCell g;
    pg_gen_groove(r, 1, g);
    CHECK(g.len == 1);
    CHECK(g.rank_of_slot[0] == 0);
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build 2>&1 | tail -5`
Expected: compile FAILURE — `GrooveCell` and `pg_gen_groove` not declared.

- [ ] **Step 3: Implement** — append to `engine/mod/phrase_gen.h` before the closing namespace brace:

```cpp
// One groove cell per phrase (spec 2026-07-16-rhythm-groove-design.md §1):
// rank_of_slot[i] = firing order of cell slot i (0 = the first note DENSE
// reveals), note_len[i] = composed note length in slots. Period = motif
// length L, tiled across the phrase like the pitch motifs, truncated over
// the tail. The downbeat anchor (slot 0) is pinned to rank 0 by construction.
struct GrooveCell {
    uint8_t rank_of_slot[32] = {};
    uint8_t note_len[32] = {};
    uint8_t len = 1;   // L, >= 1
};

// Compose the groove. Syncopation comes from displacement: a strong beat may
// hand its emphasis to the off-beat before it (a push/anticipation), which
// then OUTRANKS the beat it anticipates. Draw order is fixed (sync degree,
// one draw per push candidate, jitter, lengths) and the draw count depends
// only on L, so determinism is stable across outcomes.
inline void pg_gen_groove(Rng& rng, int L, GrooveCell& out) {
    if (L < 1) L = 1;
    if (L > 32) L = 32;
    out.len = static_cast<uint8_t>(L);

    float score[33];
    for (int i = 0; i < L; ++i) score[i] = pg_metric_weight(i);

    // Phrase-wide syncopation degree (mild -> spicy). Tuned by ear.
    float sync = 0.15f + 0.60f * rng.next_unipolar();

    // Pushes: every strong beat (even slot; s == L is the NEXT cell's wrapped
    // downbeat) may displace onto the off-beat before it.
    for (int s = 2; s <= L; s += 2) {
        bool push = rng.next_unipolar() < sync;      // always drawn: fixed count
        if (!push) continue;
        float beat_w = (s == L) ? 1.0f : score[s];
        score[s - 1] = beat_w + 0.05f;               // anticipation outranks its beat
        if (s < L) score[s] *= 0.35f;                // the displaced beat recedes
    }

    // Seeded jitter for tie-breaking variety; slot 0 pinned above everything
    // (spec: anchor rank 0 is enforced, not emergent).
    for (int i = 1; i < L; ++i) score[i] += (rng.next_unipolar() - 0.5f) * 0.06f;
    score[0] = 2.0f;

    // Stable insertion sort, descending score -> firing order.
    uint8_t order[32];
    for (int i = 0; i < L; ++i) order[i] = static_cast<uint8_t>(i);
    for (int i = 1; i < L; ++i) {
        uint8_t o = order[i];
        int j = i - 1;
        while (j >= 0 && score[order[j]] < score[o]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = o;
    }
    for (int r = 0; r < L; ++r) out.rank_of_slot[order[r]] = static_cast<uint8_t>(r);

    // Note lengths, biased short (staccato common, sustains rare). Tuned by ear.
    for (int i = 0; i < L; ++i) {
        float u = rng.next_unipolar();
        out.note_len[i] = static_cast<uint8_t>(u < 0.55f ? 1 : u < 0.80f ? 2 : u < 0.95f ? 3 : 4);
    }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="groove:*"`
Expected: 4 test cases PASS, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp
git commit -m "feat(phrase_gen): GrooveCell + pg_gen_groove — composed syncopated firing order

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Gates become all-true in `pg_gen_motif`

**Files:**
- Modify: `engine/mod/phrase_gen.h` (`pg_gen_motif`, all cases; function comment)
- Modify: `tests/test_phrase_gen.cpp` (the `any_rest` check in "generate_phrase: TwoMotif shows motivic repetition, deterministic")

**Interfaces:**
- Consumes: nothing new.
- Produces: `pg_gen_motif`/`generate_phrase`/`regenerate_unit` keep their exact signatures; `gate[]` output is now always all-true (rests come from the groove at play time — Task 3). Callers relying on `gate[]` rests must migrate to groove ranks.

- [ ] **Step 1: Update the test to the new contract** — in `tests/test_phrase_gen.cpp`, replace

```cpp
    // has at least one rest somewhere (gate layer active)
    bool any_rest = false; for (int i = 0; i < 32; ++i) any_rest |= !ga[i];
    CHECK(any_rest);
```

with

```cpp
    // gates are all-true: rests come from the groove ranking at play time
    for (int i = 0; i < 32; ++i) CHECK(ga[i]);
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="generate_phrase: TwoMotif*"`
Expected: FAIL — old metric-threshold gates still produce rests.

- [ ] **Step 3: Implement** — in `engine/mod/phrase_gen.h`, `pg_gen_motif`:

Update the function comment (`// Generate one motif's content...`) to:

```cpp
// Generate one motif's content (pitch; gate is all-true — rhythm lives in the
// GrooveCell ranking, not per-slot rests). CallResponse role is by id parity.
```

Then in each case replace the gate loop:
- `Ostinato`: `for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.30f;` → `for (int i = 0; i < L; ++i) gate[i] = true;`
- `Hierarchical`: delete `bool cellg[4];` and `for (int i = 0; i < cl; ++i) cellg[i] = pg_metric_weight(i) >= 0.25f;`, and change the tile loop to `for (int i = 0; i < L; ++i) { pitch[i] = cell[i % cl]; gate[i] = true; }`
- `CallResponse`: `for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.25f;` → `for (int i = 0; i < L; ++i) gate[i] = true;`
- `TwoMotif`/`OneMotif`/default: same replacement → `for (int i = 0; i < L; ++i) gate[i] = true;`

- [ ] **Step 4: Run the phrase_gen suite**

Run: `./build/spky_tests.exe -tc="*phrase*,*groove*,*metric*,*arrangement*,*contour*,*regenerate*,*layout*"` (after `cmake --build build`)
Expected: all PASS (the Ostinato `on >= 16` check passes trivially; `regenerate_unit gates match` passes with all-true gates).

- [ ] **Step 5: Pin `tests/test_instrument.cpp` to DENSE 0** — with gates all-true, the old
weak-beat rests that kept "instrument: voice setters and manual trigger reach the part"
silent during its settle window are gone; the natural next note moves from step 2 to
step 1. Pinning DENSE to 0 restores "only the downbeat fires" under BOTH the old
metric mask (threshold 1.0 → slot 0 only) and the upcoming groove ranking (k=1 →
anchor only). Replace the comment block directly above the test (the six lines
starting `// PROBABILITY used to force a permanent freeze ...`) with:

```cpp
// DENSE 0 leaves only the downbeat/anchor slot able to fire, so after the
// guaranteed first-sample fire (STEP entry: step -1 -> 0) the next natural
// note is a full cycle away. Settle past that single note's decay before
// checking silence, so the manual trigger is the only voice left.
```

and add one line after `inst.set_step(PART_A, true, 8);`:

```cpp
    inst.set_density(PART_A, 0.f);          // anchor-only: next natural fire is a cycle away
```

- [ ] **Step 6: Run the FULL suite to catch downstream fallout early**

Run: `./build/spky_tests.exe`
Expected: all PASS. Rationale: `_density_pass` (metric threshold) is still in place, so lane-level density behavior is unchanged by this task; only `gate[]` rests disappeared, which the lane treated as extra rests on weak beats — `test_gate_density` ("thin < full") still holds because density masking still works. If anything fails here, STOP and report — do not patch tests beyond what this plan specifies.

- [ ] **Step 7: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp tests/test_instrument.cpp
git commit -m "refactor(phrase_gen): motif gates all-true — rhythm moves to the groove cell

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Lane integration — DENSE = ranking depth

**Files:**
- Modify: `engine/mod/lane.h` (member + declarations + `set_density` comment)
- Modify: `engine/mod/lane.cpp` (`init`, `process` wrap-regen, `_effective_gate`, remove `_density_pass`, add `_groove_k`)
- Modify: `tests/test_gate_density.cpp` (rewrite density tests around groove semantics)

**Interfaces:**
- Consumes: `GrooveCell`, `pg_gen_groove(Rng&, int L, GrooveCell&)` from Task 1.
- Produces: `ModLane` private members `GrooveCell _groove;` and `int _groove_k() const;` — Task 4 reads `_groove.note_len` and calls `_effective_gate` for distance scans. `_effective_gate(slot)` semantics: melodic → `rank_of_slot[slot % len] < k`; non-melodic → `_gate[slot]` (all-true). `_density_pass` is DELETED.

- [ ] **Step 1: Rewrite `tests/test_gate_density.cpp`** — replace the whole file with:

```cpp
#include <doctest/doctest.h>
#include "mod/lane.h"
#include <set>

using namespace spky;

static ModLane melodic_step(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);           // variation defaults to 0 (LOOP)
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(2.0f);             // one cycle = 24000 samples
    return l;
}

// Which step indices fire over one full cycle. At rate 2 Hz / 16 steps a step
// is 1500 samples; l.phase() just after a fire identifies the entered step.
static std::set<int> fired_step_set(ModLane& l, int steps, int samples) {
    std::set<int> out;
    for (int n = 0; n < samples; ++n) {
        l.process();
        if (l.fired()) out.insert(static_cast<int>(l.phase() * steps) % steps);
    }
    return out;
}

TEST_CASE("DENSE is monotonic: raising density only adds notes to the groove") {
    const float densities[] = {0.05f, 0.3f, 0.6f, 1.0f};
    std::set<int> prev;
    for (int d = 0; d < 4; ++d) {
        ModLane l = melodic_step(0x11, 16);
        l.set_density(densities[d]);
        auto s = fired_step_set(l, 16, 24000);
        for (int step : prev) CHECK(s.count(step) == 1);  // superset of the sparser set
        CHECK(s.size() >= prev.size());
        prev = s;
    }
    CHECK(prev.size() == 16);        // density 1 -> every step fires
}

TEST_CASE("DENSE 0 leaves exactly the cell anchors") {
    ModLane l = melodic_step(0x11, 16);   // n=16 -> 2 instances of L=8
    l.set_density(0.f);
    auto s = fired_step_set(l, 16, 24000);
    CHECK(s == std::set<int>{0, 8});      // slot 0 of each instance (rank 0)
}

TEST_CASE("DENSE is reversible: density 1 == the full pattern") {
    ModLane a = melodic_step(0x11, 16);
    ModLane b = melodic_step(0x11, 16);
    b.set_density(0.2f);             // thin...
    b.set_density(1.0f);             // ...then restore (never edits the groove)
    CHECK(fired_step_set(a, 16, 24000) == fired_step_set(b, 16, 24000));
}

TEST_CASE("FLOW never freezes after PROBABILITY removal") {
    ModLane l;
    l.set_melodic(true);
    l.init(48000.f, 0x22);
    l.set_shape(0.5f);
    l.set_step(false, 8);            // FLOW: no per-step gate => no freeze source
    l.set_rate_hz(3.0f);
    bool ever_frozen = false;
    for (int n = 0; n < 48000; ++n) { l.process(); ever_frozen |= l.frozen(); }
    CHECK_FALSE(ever_frozen);
}
```

- [ ] **Step 2: Run to verify the new tests fail**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ./build/spky_tests.exe -tc="DENSE*"`
Expected: FAIL — "DENSE 0 leaves exactly the cell anchors" fails under the old metric-threshold mask (old density 0 fires only step 0, not {0, 8}); monotonic may partially pass but the density-1 == 16-steps check fails only if old gates rest (they're all-true after Task 2, so it passes) — the anchors test is the definitive red.

- [ ] **Step 3: Implement lane changes**

`engine/mod/lane.h`:
- Change the `set_density` comment to: `// 0..1 -> how deep into the groove ranking (k of L cell notes)`
- Replace the declaration `bool _density_pass(int slot) const;    // metric-weight threshold from DENSITY` with `int _groove_k() const;              // DENSE -> how many ranked cell notes play`
- Add to the private members, after `PhraseLayout _layout;`:

```cpp
    GrooveCell _groove;
```

`engine/mod/lane.cpp`:
- In `init()`, after `generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);` add:

```cpp
        pg_gen_groove(_rng, _layout.motif_len, _groove);
```

- In `process()`, inside the wrap-regen branch, after the `generate_phrase(...)` line add the same call:

```cpp
            pg_gen_groove(_rng, _layout.motif_len, _groove);
```

- Replace `_density_pass` with:

```cpp
int ModLane::_groove_k() const {
    int L = _groove.len < 1 ? 1 : _groove.len;
    int k = static_cast<int>(std::lround(_density * static_cast<float>(L)));
    if (k < 1) k = 1;              // the anchor is unmaskable
    if (k > L) k = L;
    return k;
}
```

- Replace `_effective_gate` with:

```cpp
bool ModLane::_effective_gate(int slot) const {
    if (!_melodic) return _gate[slot];   // non-melodic lanes: all-true, DENSE unrouted
    return _groove.rank_of_slot[slot % _groove.len] < _groove_k();
}
```

- [ ] **Step 4: Run the full suite**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: ALL PASS. Notes for triage if not:
- `test_instrument.cpp` "voice setters and manual trigger" was pinned to DENSE 0 in Task 2 — anchor-only under the groove too; must pass unchanged.
- `test_part.cpp` "decay length follows the master cycle" already sets density 0 with steps=8 (single cell) — anchor-only behavior is identical; must pass unchanged.
- `test_variation.cpp` all cases assert relative properties (identical cycles at LOOP, same fire count under GROW, changed pitches under RENEW, bit-identical reruns) — all hold under the groove; must pass unchanged.
- `test_new_phrase.cpp` compares fired-value sequences across regen — holds (groove re-rolls with the phrase); must pass unchanged.
- Any other failure: STOP and report; do not improvise test edits.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_gate_density.cpp
git commit -m "feat(lane): DENSE = groove ranking depth — monotonic syncopated note reveal

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Composed note durations on the GATE output

**Files:**
- Modify: `engine/mod/lane.h` (`gate_state`, new members, `_start_note` declaration)
- Modify: `engine/mod/lane.cpp` (`init`, `reset`, `_on_boundary`, new `_start_note`)
- Test: `tests/test_gate_density.cpp` (append)

**Interfaces:**
- Consumes: `_groove.note_len[]`, `_effective_gate(slot)` from Task 3.
- Produces: `gate_state()` new semantics — melodic STEP: high while the current composed note sustains (`_note_age < _note_hold`); non-melodic STEP and all FLOW: always true. `SuperModulator::pitch_gate()` and the VCV GATE output inherit this with no changes.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_gate_density.cpp` (add `#include <vector>` to the includes):

```cpp
TEST_CASE("gate releases before the next note when the gap is long") {
    ModLane l = melodic_step(0x11, 16);
    l.set_density(0.f);                    // notes at steps 0 and 8 only (L=8)
    const int step_samples = 1500;         // 24000-sample cycle / 16 steps
    std::vector<char> gate;
    for (int n = 0; n < 24000; ++n) { l.process(); gate.push_back(l.gate_state()); }
    CHECK(gate[10]);                       // note sounding just after the downbeat
    // note_len is capped at 4 < the 8-step gap, so the gate MUST fall in between
    CHECK_FALSE(gate[7 * step_samples + 10]);
    int run = 0;                           // high run from the downbeat: 1..4 steps
    while (run < 24000 && gate[run]) ++run;
    CHECK(run >= 1 * step_samples - 2);
    CHECK(run <= 4 * step_samples + 2);
}

TEST_CASE("adjacent notes tie: gate high across a run of gated steps") {
    ModLane l = melodic_step(0x11, 16);
    l.set_density(1.f);                    // every step fires -> continuous tie
    l.process();                           // enter step 0 (first fire)
    bool always_high = true;
    for (int n = 1; n < 24000; ++n) { l.process(); always_high &= l.gate_state(); }
    CHECK(always_high);
}

TEST_CASE("gate can sustain across a frozen (rest) step") {
    // k = 7 of 8: one rest slot per cell. Whenever the note before it has
    // note_len >= 2 the gate bridges the rest (high while frozen). Statistical
    // over seeds: bridging must be common (P(len>=2) = 0.45 per phrase).
    int bridged = 0;
    for (uint32_t seed = 1; seed <= 40; ++seed) {
        ModLane l = melodic_step(seed * 31u, 16);
        l.set_density(0.9f);               // k=7 of 8
        bool high_while_frozen = false;
        for (int n = 0; n < 24000; ++n) {
            l.process();
            if (l.frozen() && l.gate_state()) high_while_frozen = true;
        }
        if (high_while_frozen) ++bridged;
    }
    CHECK(bridged >= 8);                   // expected ~18/40
}
```

- [ ] **Step 2: Run to verify failures**

Run: `cmake --build build && ./build/spky_tests.exe -tc="gate*,adjacent*"`
Expected: "gate can sustain across a frozen (rest) step" FAILS with `bridged == 0` — at Task-3 state `gate_state()` mirrors the current slot's rank gate, so it is never high while frozen. The other two new cases happen to pass already (a 1-step rank gate satisfies the 1..4-step run window, and density 1 keeps the gate high) — that is expected; the frozen-bridge case is the definitive red. Do not proceed unless it is red.

- [ ] **Step 3: Implement**

`engine/mod/lane.h`:
- Replace the `gate_state()` inline with:

```cpp
    // GATE: melodic STEP sustains the composed note (age < hold); else high.
    bool  gate_state() const { return _step_mode ? (!_melodic || _note_age < _note_hold) : true; }
```

- Add after `void  _renew_walk();` declaration:

```cpp
    void  _start_note(int slot);    // groove: set _note_hold (tie-capped) on fire
```

- Add members after `bool  _frozen = false;`:

```cpp
    int   _note_age  = 0;    // steps since the current note fired
    int   _note_hold = 0;    // composed note length (capped at the next note)
```

`engine/mod/lane.cpp`:
- In `init()`, after `_frozen = false;` add:

```cpp
    _note_age = 0;
    _note_hold = 0;
```

- In `reset()`, after `_cur_step = -1;` add:

```cpp
    _note_age = 0;
    _note_hold = 0;
```

- Replace `_on_boundary()` with:

```cpp
void ModLane::_on_boundary() {
    int slot = _sh_slot();
    // STEP consults the effective gate (groove rank vs DENSE); FLOW has no
    // per-step gate so it always fires (no freeze source after PROBABILITY).
    bool gated = _step_mode ? _effective_gate(slot) : true;
    _frozen = !gated;
    if (gated) {
        _fired = true;
        if (_melodic && _step_mode) _start_note(slot);
        if (_variation > 0.f) _mutate_slot(slot);  // GROW pitch
        _target = _compute_raw();
    } else {
        ++_note_age;   // rest step: the running note ages toward its release
    }
    // if !gated: hold the previous _target (frozen) — and the buffer slot with it
}

void ModLane::_start_note(int slot) {
    int n = _steps > kSeqSlots ? kSeqSlots : _steps;   // effective phrase length
    if (n < 1) n = 1;
    int dist = 1;                                       // steps to the next note
    while (dist < n && !_effective_gate((slot + dist) % n)) ++dist;
    int hold = static_cast<int>(_groove.note_len[slot % _groove.len]);
    _note_hold = hold > dist ? dist : hold;             // reaching the next note = tie
    _note_age = 0;
}
```

- [ ] **Step 4: Run the full suite**

Run: `cmake --build build && ./build/spky_tests.exe`
Expected: ALL PASS. The three new cases pass; nothing else consumes `gate_state()` in tests except through instrument silence checks already pinned to density 0 (note at the anchor releases after ≤4 of 8 steps — still silent before the manual trigger; the 10000-sample settle in `test_instrument.cpp` stays valid since hold ≤ 4 steps and voice decay was already the binding constraint).

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_gate_density.cpp
git commit -m "feat(lane): composed note durations — GATE sustains, ties, and rests

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Full verification + render audition

**Files:**
- No source changes expected. Artifacts to scratchpad only.

**Interfaces:**
- Consumes: everything above.
- Produces: green full suite, a rendered WAV for the listening pass, clean git status.

- [ ] **Step 1: Full clean test run**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: `100% tests passed`.

- [ ] **Step 2: Render the melody demo scenario for audition**

Run: `./build/render.exe host/render/scenarios/demo_step_melody.json "$SCRATCHPAD/groove_demo.wav" "$SCRATCHPAD/groove_demo.csv"` (use the session scratchpad path; render usage: `render <scenario.json> [out.wav] [mods.csv]`)
Expected: exits 0, WAV written, non-silent (spot-check: file size > 100 KB).

- [ ] **Step 3: Confirm clean state and report**

Run: `git status --short && git log --oneline -6`
Expected: no unstaged changes; the four feature commits on `main`. Report the WAV path for the human listening pass (Rack play test remains deferred, per milestone memory).
