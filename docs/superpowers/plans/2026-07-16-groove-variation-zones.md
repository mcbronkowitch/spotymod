# Groove Variation Zones Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The MELODY (VARIATION) knob reaches the rhythm in its outer zone: `|variation|` 0…0.25 = melody-only (today's behavior), 0.25…1.0 = groove mutations ramp in (GROW: adjacent-rank swap / length nudge; RENEW: push flip / length nudge / extreme full re-roll), at most one attempt per cycle wrap.

**Architecture:** Two pure mutator functions on `GrooveCell` in `phrase_gen.h` (unit-testable, invariant-preserving: permutation, anchor rank 0, lengths 1..4), plus lane-side zoning/dice in a new `ModLane::_mutate_groove(bool renew_side)` called from both variation branches of the wrap block. Spec: `docs/superpowers/specs/2026-07-16-groove-variation-zones-design.md`.

**Tech Stack:** C++17 header-mostly engine, doctest, CMake+Ninja+clang.

## Global Constraints

- Work directly on `main` (established user instruction; no worktree).
- Engine code: no heap, no exceptions, no time/global RNG — all draws through the lane's `spky::Rng`; the zone dice is ALWAYS drawn when its side is active (fixed base draw count per wrap), mutation-internal draws are conditional (like `regenerate_unit`).
- Invariants every mutation must preserve: `rank_of_slot` permutation of `0..L−1`, slot 0 keeps rank 0, `note_len` in `[1,4]`. DENSE monotonicity follows from these.
- Zone/tuning constants live in `lane.cpp`, ear-tunable: `kGrooveVarStart = 0.25f`, `kGrooveRerollGate = 0.9f`, `kGrooveRerollProb = 0.25f`. Tests assert behavior, not these values (except the zone-1 contract at 0.2, safely inside zone 1).
- Build/test (Bash tool): `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build` then `./build/spky_tests.exe` (doctest filter: `-tc="<pattern>"`).
- Every commit message ends with: `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` (never the default Claude trailer).

---

### Task 1: Groove mutators in phrase_gen

**Files:**
- Modify: `engine/mod/phrase_gen.h` (append after `pg_gen_groove`, before the closing namespace brace)
- Test: `tests/test_phrase_gen.cpp` (append)

**Interfaces:**
- Consumes: `GrooveCell`, `pg_gen_groove(Rng&, int L, GrooveCell&)`, `Rng::next_unipolar()`.
- Produces: `void pg_groove_nudge_len(Rng&, GrooveCell&)`, `void pg_groove_mutate_grow(Rng&, GrooveCell&)`, `void pg_groove_mutate_renew(Rng&, GrooveCell&, bool reroll)` — Task 2 calls the latter two from the lane.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_phrase_gen.cpp`:

```cpp
// --- groove variation-zone mutators ---

static bool gv_is_perm_anchor0(const GrooveCell& g) {
    bool seen[32] = {};
    for (int i = 0; i < g.len; ++i) {
        if (g.rank_of_slot[i] >= g.len) return false;
        seen[g.rank_of_slot[i]] = true;
    }
    for (int i = 0; i < g.len; ++i) if (!seen[i]) return false;
    return g.rank_of_slot[0] == 0;
}

TEST_CASE("groove mutators preserve permutation, anchor, and length bounds") {
    for (int L : {1, 2, 7, 8}) {
        for (uint32_t seed = 1; seed <= 50; ++seed) {
            Rng r; r.seed(seed * 0x9E3779B9u + static_cast<uint32_t>(L));
            GrooveCell g;
            pg_gen_groove(r, L, g);
            for (int step = 0; step < 8; ++step) {
                if (step % 2 == 0) pg_groove_mutate_grow(r, g);
                else pg_groove_mutate_renew(r, g, /*reroll=*/step == 7);
                REQUIRE(gv_is_perm_anchor0(g));
                for (int i = 0; i < g.len; ++i) {
                    REQUIRE(g.note_len[i] >= 1);
                    REQUIRE(g.note_len[i] <= 4);
                }
            }
        }
    }
}

TEST_CASE("mutate_grow changes at most one thing") {
    for (uint32_t seed = 1; seed <= 100; ++seed) {
        Rng r; r.seed(seed * 2654435761u);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        GrooveCell before = g;
        pg_groove_mutate_grow(r, g);
        int rank_diffs = 0, len_diffs = 0;
        for (int i = 0; i < 8; ++i) {
            if (g.rank_of_slot[i] != before.rank_of_slot[i]) ++rank_diffs;
            if (g.note_len[i] != before.note_len[i]) ++len_diffs;
        }
        // nothing, or one adjacent-rank swap (two slots), or one length +/-1
        CHECK((rank_diffs == 0 || rank_diffs == 2));
        CHECK(len_diffs <= 1);
        CHECK((rank_diffs == 0 || len_diffs == 0));   // never both
        if (rank_diffs == 2) {                        // swapped ranks are adjacent, anchor excluded
            int ra = -1, rb = -1;
            for (int i = 0; i < 8; ++i) if (g.rank_of_slot[i] != before.rank_of_slot[i]) {
                if (ra < 0) ra = before.rank_of_slot[i]; else rb = before.rank_of_slot[i];
            }
            if (ra > rb) { int t = ra; ra = rb; rb = t; }
            CHECK(rb == ra + 1);
            CHECK(ra >= 1);
        }
        if (len_diffs == 1) {
            for (int i = 0; i < 8; ++i) {
                int d = static_cast<int>(g.note_len[i]) - static_cast<int>(before.note_len[i]);
                CHECK(d >= -1); CHECK(d <= 1);
            }
        }
    }
}

TEST_CASE("mutate_renew push flip swaps an off-beat with its even beat") {
    int flips = 0;
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        Rng r; r.seed(seed * 0xC0FFEEu);
        GrooveCell g;
        pg_gen_groove(r, 8, g);
        GrooveCell before = g;
        pg_groove_mutate_renew(r, g, false);
        int diffs = 0, slots[2] = {-1, -1};
        for (int i = 0; i < 8; ++i) if (g.rank_of_slot[i] != before.rank_of_slot[i]) {
            if (diffs < 2) slots[diffs] = i;
            ++diffs;
        }
        CHECK(diffs <= 2);
        if (diffs == 2) {                             // a flip: slots are s-1, s with s even <= L-2
            ++flips;
            int lo = slots[0] < slots[1] ? slots[0] : slots[1];
            int hi = slots[0] < slots[1] ? slots[1] : slots[0];
            CHECK(hi == lo + 1);
            CHECK(hi % 2 == 0);
            CHECK(hi <= 6);
            CHECK(lo >= 1);
        }
    }
    CHECK(flips > 200 * 3 / 10);                      // ~70% of mutations are flips
}

TEST_CASE("groove mutators are deterministic per seed") {
    Rng a; a.seed(0xABCu);
    Rng b; b.seed(0xABCu);
    GrooveCell ga, gb;
    pg_gen_groove(a, 8, ga);
    pg_gen_groove(b, 8, gb);
    for (int i = 0; i < 6; ++i) {
        pg_groove_mutate_grow(a, ga);
        pg_groove_mutate_grow(b, gb);
        pg_groove_mutate_renew(a, ga, i == 5);
        pg_groove_mutate_renew(b, gb, i == 5);
    }
    for (int i = 0; i < 8; ++i) {
        CHECK(ga.rank_of_slot[i] == gb.rank_of_slot[i]);
        CHECK(ga.note_len[i] == gb.note_len[i]);
    }
}
```

- [ ] **Step 2: Run to verify compile failure**

Run: `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build 2>&1 | tail -5`
Expected: compile FAILURE — `pg_groove_mutate_grow` etc. not declared.

- [ ] **Step 3: Implement** — append to `engine/mod/phrase_gen.h` after `pg_gen_groove`, before the closing namespace brace:

```cpp
// --- Groove variation-zone mutators (spec 2026-07-16-groove-variation-zones) ---
// All preserve the GrooveCell invariants: rank permutation, slot-0 anchor at
// rank 0, note_len in [1,4]. Called by the lane at cycle wraps only; the lane
// owns the zoning dice. Draws are conditional (like regenerate_unit).

// Shared: pick one slot, nudge its composed length by +/-1 (clamped).
inline void pg_groove_nudge_len(Rng& rng, GrooveCell& g) {
    const int L = g.len;
    int i = static_cast<int>(rng.next_unipolar() * static_cast<float>(L));
    if (i > L - 1) i = L - 1;
    int v = static_cast<int>(g.note_len[i]) + (rng.next_unipolar() < 0.5f ? -1 : 1);
    if (v < 1) v = 1;
    if (v > 4) v = 4;
    g.note_len[i] = static_cast<uint8_t>(v);
}

// GROW-side drift: 50/50 adjacent-rank swap (one note moves one place in the
// order DENSE reveals notes; rank 0 excluded) or length nudge.
inline void pg_groove_mutate_grow(Rng& rng, GrooveCell& g) {
    const int L = g.len;
    if (rng.next_unipolar() < 0.5f) {
        if (L < 4) return;                            // no swappable pair beside the anchor
        int j = 1 + static_cast<int>(rng.next_unipolar() * static_cast<float>(L - 2));
        if (j > L - 2) j = L - 2;                     // swap ranks j <-> j+1, j in 1..L-2
        int s1 = -1, s2 = -1;
        for (int i = 0; i < L; ++i) {
            if (g.rank_of_slot[i] == j)     s1 = i;
            if (g.rank_of_slot[i] == j + 1) s2 = i;
        }
        g.rank_of_slot[s1] = static_cast<uint8_t>(j + 1);
        g.rank_of_slot[s2] = static_cast<uint8_t>(j);
    } else {
        pg_groove_nudge_len(rng, g);
    }
}

// RENEW-side re-decision: 70% push flip — swap the off-beat s-1 with its even
// beat s, the exact semantic toggle of pg_gen_groove's displacement (s == L,
// the wrapped downbeat, is excluded: flipping it would demote the anchor) —
// else length nudge. `reroll` regenerates the whole cell instead.
inline void pg_groove_mutate_renew(Rng& rng, GrooveCell& g, bool reroll) {
    const int L = g.len;
    if (reroll) { pg_gen_groove(rng, L, g); return; }
    if (rng.next_unipolar() < 0.7f) {
        int nc = (L - 2) / 2;                         // candidates s = 2, 4, ..., <= L-2
        if (nc < 1) return;
        int c = static_cast<int>(rng.next_unipolar() * static_cast<float>(nc));
        if (c > nc - 1) c = nc - 1;
        int s = 2 + 2 * c;
        uint8_t t = g.rank_of_slot[s - 1];
        g.rank_of_slot[s - 1] = g.rank_of_slot[s];
        g.rank_of_slot[s] = t;
    } else {
        pg_groove_nudge_len(rng, g);
    }
}
```

- [ ] **Step 4: Run the new tests**

Run: `cmake --build build && ./build/spky_tests.exe -tc="*mutat*,groove*"`
Expected: all PASS (the pre-existing `groove:*` cases stay green — `pg_gen_groove` itself is untouched).

- [ ] **Step 5: Run the full suite**

Run: `./build/spky_tests.exe`
Expected: ALL PASS (nothing else consumes the new functions yet).

- [ ] **Step 6: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp
git commit -m "feat(phrase_gen): groove mutators — rank drift, push flip, length nudge

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Lane zoning — VARIATION's outer zone reaches the groove

**Files:**
- Modify: `engine/mod/lane.h` (one declaration)
- Modify: `engine/mod/lane.cpp` (constants, `_mutate_groove`, two call sites in the wrap block)
- Test: `tests/test_variation.cpp` (append)

**Interfaces:**
- Consumes: `pg_groove_mutate_grow(Rng&, GrooveCell&)`, `pg_groove_mutate_renew(Rng&, GrooveCell&, bool)` from Task 1; existing `_groove`, `_variation`, `_melodic`, `_step_mode`, `_rng`.
- Produces: no new public API. Behavior contract for tests: `|variation| <= kGrooveVarStart` never alters the groove; at the stops the groove drifts.

- [ ] **Step 1: Write the failing tests** — append to `tests/test_variation.cpp` (add `#include <set>` to the includes):

```cpp
// Fired-step SET per cycle (rhythm identity), split on phase wrap.
static std::vector<std::set<int>> fired_sets(ModLane& l, int steps, int cycles) {
    std::vector<std::set<int>> out(1);
    float prev = l.phase();
    int wraps = 0;
    for (int n = 0; n < 400000 && wraps <= cycles; ++n) {
        l.process();
        float ph = l.phase();
        if (ph < prev) { out.emplace_back(); ++wraps; }
        prev = ph;
        if (l.fired()) out.back().insert(static_cast<int>(ph * steps) % steps);
    }
    return out;
}

TEST_CASE("groove zone 1: variation up to 0.25 never touches the rhythm") {
    for (float v : {0.2f, -0.2f, 0.25f}) {
        ModLane l = make_melodic_step_lane(0x77, 16);
        l.set_density(0.6f);
        l.set_variation(v);
        auto cy = fired_sets(l, 16, 8);
        REQUIRE(cy.size() >= 8);
        for (size_t c = 2; c < 8; ++c) CHECK(cy[1] == cy[c]);   // pitch may move; rhythm must not
    }
}

TEST_CASE("groove zone 2: at the stops the rhythm drifts") {
    for (float v : {1.0f, -1.0f}) {
        ModLane l = make_melodic_step_lane(0x77, 16);
        l.set_density(0.6f);
        l.set_variation(v);
        auto cy = fired_sets(l, 16, 16);
        REQUIRE(cy.size() >= 16);
        bool changed = false;
        for (size_t c = 2; c < 16; ++c) if (cy[c] != cy[1]) changed = true;
        CHECK(changed);
    }
}
```

NOTE on the zone-2 test: whether the fired SET changes within 15 cycles depends on the seed (swaps/flips only alter the set when they cross the DENSE boundary; length nudges never do). The assertions are deterministic for a fixed seed — run once; if a stop does not flip within 15 cycles for seed 0x77, raise the cycle count (up to 32) or pick a nearby seed for BOTH cases and note it in a comment. Do not weaken the assertion itself.

- [ ] **Step 2: Run to verify the expected red**

Run: `cmake --build build && ./build/spky_tests.exe -tc="groove zone*"`
Expected: "groove zone 1" PASSES already (nothing mutates the groove today — that is the pre-change behavior); "groove zone 2" FAILS (`changed == false`) because the groove is currently immutable under VARIATION. Zone 2 red is the gate; do not proceed without it.

- [ ] **Step 3: Implement**

`engine/mod/lane.h` — add after the `void  _renew_walk();` declaration:

```cpp
    void  _mutate_groove(bool renew_side);  // VARIATION outer zone: rhythm dice (wrap only)
```

`engine/mod/lane.cpp` — add to the constants at the top (after `kGravity`):

```cpp
static constexpr float kGrooveVarStart   = 0.25f;  // |variation| below: melody only
static constexpr float kGrooveRerollGate = 0.9f;   // RENEW near the stop may re-roll all
static constexpr float kGrooveRerollProb = 0.25f;  // ...with this chance, when the dice hits
```

Add the method (near `_renew_units`):

```cpp
void ModLane::_mutate_groove(bool renew_side) {
    if (!_melodic || !_step_mode) return;
    float a = _variation < 0.f ? -_variation : _variation;
    float r = (a - kGrooveVarStart) / (1.f - kGrooveVarStart);
    if (r < 0.f) r = 0.f;
    if (r > 1.f) r = 1.f;
    // Dice always drawn while this side is active: fixed base draw count per
    // wrap; in zone 1 (r == 0) it can never pass.
    if (_rng.next_unipolar() >= r * r) return;
    if (renew_side) {
        bool reroll = a >= kGrooveRerollGate && _rng.next_unipolar() < kGrooveRerollProb;
        pg_groove_mutate_renew(_rng, _groove, reroll);
    } else {
        pg_groove_mutate_grow(_rng, _groove);
    }
}
```

In `process()`'s wrap block, extend both variation branches (draw order: after the existing draws of each side):

```cpp
        if (_variation > 0.f) {                 // GROW: EVOLVE contour walk (live)
            _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _variation, -0.5f, 0.5f);
            _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _variation, -0.25f, 0.25f);
            _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _variation, -0.2f, 0.2f);
            _mutate_groove(false);              // outer zone: rhythm drifts too
        } else if (_variation < 0.f) {          // RENEW: per-unit regen + walk decay
            if (_melodic && _step_mode) _renew_units();
            else if (!_melodic) {
                if (_rng.next_unipolar() < _variation * _variation) _renew_walk();
            }
            float decay = 1.f + 0.2f * _variation;  // variation -1 -> x0.8/cycle
            _ev_phase *= decay; _ev_shape *= decay; _ev_rate *= decay;
            _mutate_groove(true);               // outer zone: re-decide pushes
        }                                       // variation 0 (LOOP): walk frozen
```

- [ ] **Step 4: Run the zone tests, then the full suite**

Run: `cmake --build build && ./build/spky_tests.exe -tc="groove zone*"` — expect both PASS (apply the zone-2 NOTE from Step 1 if the fixed seed needs more cycles; re-verify zone 1 after any seed change).
Then: `./build/spky_tests.exe` — expect ALL PASS. Triage notes:
- `test_variation.cpp` "GROW varies pitch ... keeps the same gate rhythm" must still pass: it compares cycle-1 fire COUNTS at density 1 (default), where every step fires regardless of rank swaps, and rank swaps never change the count at any fixed density anyway.
- "RENEW at -1 replaces units every cycle" must still pass (values change; count at density 1 stays 16).
- "determinism: identical drive" must still pass (same drive → same draws).
- Any other failure: STOP and report with output; do not improvise test edits.

- [ ] **Step 5: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/lane.h engine/mod/lane.cpp tests/test_variation.cpp
git commit -m "feat(lane): MELODY outer zone reaches the groove — staged rhythm variation

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Full verification + audition render

**Files:** none (verification only; render artifact to scratchpad).

- [ ] **Step 1:** `source env.sh && cmake --build build && ctest --test-dir build --output-on-failure` → `100% tests passed`.
- [ ] **Step 2:** Render `host/render/scenarios/demo_step_melody.json` to the session scratchpad (`render <scenario.json> [out.wav] [mods.csv]`) — the scenario's GROW section (t=6–14 at +0.6) and RENEW section (t=20–28 at −0.9) now exercise zone 2; confirm non-silent output and, from the CSV `a_pgate` column, that the fired pattern in the RENEW window differs across cycles while the t=0–5 window (variation 0) loops identically.
- [ ] **Step 3:** `git status --short` clean; report the WAV path for the human listening pass.
