# Spotykach Melody Engine Rework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the entropy/probability/capture sequencer with a Marbles-style MELODY knob — one bipolar `set_variation(−1..+1)` control (RENEW ← LOOP → GROW), a motivic phrase generator with five switchable principles, a gate/rest layer with deterministic DENSITY, and a new-phrase gesture — so the PITCH lane plays composed, looping, evolving melodies instead of note salad.

**Architecture:** A new header-only, allocation-free, deterministic phrase generator (`engine/mod/phrase_gen.h`) produces contour-walk melodies assembled from repeating motifs plus per-slot gates. `ModLane` gains a gate array, a motif/layout description, and a `_melodic` flag (PITCH only); its `set_variation` drives per-slot pitch GROW (positive) and per-renewal-unit regeneration RENEW (negative). Capture, replay and PROBABILITY are removed from the entire stack (lane → super_modulator → instrument → host).

**Tech Stack:** C++17, namespace `spky`, no heap / no allocation in the audio path / no libDaisy in `engine/`. Deterministic xorshift32 `Rng` (`engine/mod/rng.h`). Tests: doctest via CMake target `spky_tests`, run with `ctest`. Desktop build: clang + Ninja.

## Global Constraints

- **Repos:** The engine code lives in the **fork** repo at `c:\Users\bernd\Documents\AI\Spotykach`. All `engine/`, `host/`, `tests/`, `CMakeLists.txt` paths below are **relative to that fork repo**. Do all code work and commits there on branch **`melody-engine-rework`**. This plan and the design spec live in the separate residency repo (`C:\Users\bernd\Documents\AI\Synthux Design Residency`); only Task 10 edits a residency-repo file.
- **Namespace:** everything in `namespace spky`.
- **Determinism is a hard invariant:** every random draw goes through the lane's seeded `Rng` (`next_unipolar()` / `next_bipolar()` / `next_u32()` — there is **no** `Rng::next()`). No time seeding, no `Math.random`. Identical scenario → bit-identical WAV. `record()`-style non-RNG writes are gone with capture.
- **No allocation / no heap / no libDaisy in `engine/`.** POD buffers only, caller-owned. `engine/` must compile with no libDaisy include.
- **Buffer sizes:** live melody buffer is `ModLane::kSeqSlots = 32` (indexed by `_sh_slot()`). The old capture ring `CaptureLoop::kSlots = 192` is deleted.
- **PITCH lane index** is `LANE_PITCH = 2` (`engine/mod/lane_id.h`); it is the only `_melodic` lane.
- **Build/test commands** (from fork repo root, git-bash):
  ```bash
  source env.sh
  cmake -S . -B build
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```
- **Every new test `.cpp` must be added to the `spky_tests` source list in `CMakeLists.txt`** (there is no glob). Deleted test files must be removed from that list.
- **Commit trailer:** end every commit message with
  `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>`
- Build stays green (`ctest` all-pass) at the end of **every** task.

---

## File Structure

| File | Responsibility |
|---|---|
| `engine/mod/phrase_gen.h` | **New.** Header-only generator: `Principle` enum, `PhraseLayout`, `generate_phrase`, `regenerate_unit`, contour-walk + metric-weight + motif-sizing + arrangement helpers. Deterministic, no heap. |
| `engine/mod/lane.h` / `lane.cpp` | `ModLane`: add gate/motif/layout/variation/density/melodic/regen state; `set_variation`/`set_density`/`set_principle`/`new_phrase`; effective-gate + RENEW mechanics; remove all capture + probability + erode. |
| `engine/mod/capture.h` | **Deleted.** |
| `engine/mod/super_modulator.{h,cpp}` | Drop capture + probability; set PITCH `_melodic`; rename entropy→variation; forward density/principle/new_phrase to PITCH. |
| `engine/instrument.{h,cpp}` | Public API: remove capture + probability; rename entropy→variation; add density/principle/new_phrase. |
| `host/render/scenario.cpp` | Remove capture + probability actions; entropy→variation (accept both names); add density/principle/new_phrase actions. |
| `host/render/main.cpp` | Remove `a_cap`/`b_cap` CSV columns; add `a_pgate`/`b_pgate` (PITCH note/rest). |
| `host/render/scenarios/*.json` | Retire capture demos; rework `demo_step_melody.json`; add a DENSITY-sweep demo. |
| `tests/test_phrase_gen.cpp` | **New.** Generator unit tests. |
| `tests/test_gate_density.cpp` | **New.** Gate/rest + DENSITY tests. |
| `tests/test_new_phrase.cpp` | **New.** Step-change + new_phrase tests. |
| `tests/test_variation.cpp` | **New** (replaces `test_entropy_seq.cpp`). LOOP/GROW/RENEW tests. |
| `tests/test_capture.cpp` | **Deleted.** |
| `tests/test_entropy_seq.cpp` | **Deleted** (superseded by `test_variation.cpp`). |
| `tests/test_lane.cpp` / `test_super_modulator.cpp` / `test_instrument.cpp` / `test_scenario.cpp` | Remove capture/probability cases; keep the rest. |
| `docs/superpowers/specs/2026-07-10-spotykach-modulation-first-synth-design.md` (residency repo) | Master-spec touch-ups (Task 10). |

---

## Task 0: Create the working branch

- [ ] **Step 1: Create and switch to the branch in the fork repo**

Run (git-bash):
```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git checkout -b melody-engine-rework
git status
```
Expected: `On branch melody-engine-rework`, clean or only the expected M4-era working changes. If there are unrelated dirty files, stop and report.

- [ ] **Step 2: Confirm the build is green before changing anything**

```bash
source env.sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```
Expected: all tests pass. This is the baseline; if it is red, stop and report.

---

## Task 1: PhraseGen core — enum, layout, helpers, sizing, arrangement

**Files:**
- Create: `engine/mod/phrase_gen.h`
- Test: `tests/test_phrase_gen.cpp`
- Modify: `CMakeLists.txt` (add the test to `spky_tests`)

**Interfaces:**
- Produces: `enum class Principle : uint8_t { TwoMotif, OneMotif, Hierarchical, CallResponse, Ostinato, kCount }`; `struct PhraseLayout { uint8_t motif_len, tail_len, inst_count, motif_count; }` (`motif_count` = number of RENEW **renewal units**, in `[1, inst_count]`); inline helpers `pg_clampf`, `pg_metric_weight(int)`, `pg_contour_walk(Rng&, float*, int, float, float, float)`, `pg_target_len(Principle)`, `pg_derive_sizing(Principle,int,int&,int&,int&)`, `pg_build_arrangement(Principle,int,uint8_t*,uint8_t*,int&,int&)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_phrase_gen.cpp`:
```cpp
#include <doctest/doctest.h>
#include "mod/phrase_gen.h"
#include "mod/rng.h"
#include <cmath>

using namespace spky;

TEST_CASE("layout invariant holds for awkward step counts") {
    for (int steps : {5, 7, 11, 13, 32, 3, 1}) {
        for (int pi = 0; pi < (int)Principle::kCount; ++pi) {
            int n = steps > 32 ? 32 : steps;
            int k, L, r;
            pg_derive_sizing((Principle)pi, n, k, L, r);
            CHECK(k >= 1);
            CHECK(L >= 1);
            CHECK(r >= 0);
            CHECK(r < (L > 1 ? L : 2));   // tail shorter than a motif
            CHECK(k * L + r == n);        // the core invariant
        }
    }
}

TEST_CASE("metric weight: downbeat strongest, binary subdivision order") {
    CHECK(pg_metric_weight(0) == doctest::Approx(1.0f));
    CHECK(pg_metric_weight(8) > pg_metric_weight(4));
    CHECK(pg_metric_weight(4) > pg_metric_weight(2));
    CHECK(pg_metric_weight(2) > pg_metric_weight(1));
    CHECK(pg_metric_weight(1) > 0.0f);
}

TEST_CASE("contour walk is a line, not independent draws, and deterministic") {
    Rng a; a.seed(0xC0FFEE);
    Rng b; b.seed(0xC0FFEE);
    float wa[16], wb[16];
    pg_contour_walk(a, wa, 16, 0.f, 0.6f, 0.12f);
    pg_contour_walk(b, wb, 16, 0.f, 0.6f, 0.12f);
    float sum_absdiff = 0.f;
    for (int i = 0; i < 16; ++i) {
        CHECK(wa[i] == doctest::Approx(wb[i]));      // determinism
        CHECK(wa[i] >= -1.0f); CHECK(wa[i] <= 1.0f); // bounded
        if (i > 0) sum_absdiff += std::fabs(wa[i] - wa[i-1]);
    }
    CHECK((sum_absdiff / 15.f) < 0.4f);              // small mean step => a line
}

TEST_CASE("arrangement: TwoMotif repeats a motif; OneMotif is one motif") {
    uint8_t moti[32], uniti[32];
    int mc, uc;
    pg_build_arrangement(Principle::TwoMotif, 4, moti, uniti, mc, uc);
    // A A B A  => id 0 appears 3x, id 1 once
    int count0 = 0; for (int j = 0; j < 4; ++j) count0 += (moti[j] == 0);
    CHECK(count0 == 3);
    CHECK(mc == 2);
    pg_build_arrangement(Principle::OneMotif, 4, moti, uniti, mc, uc);
    for (int j = 0; j < 4; ++j) CHECK(moti[j] == 0);
    CHECK(mc == 1);
}
```

- [ ] **Step 2: Run the test to verify it fails (does not compile)**

```bash
source env.sh && cmake --build build 2>&1 | head -40
```
Expected: FAIL — `mod/phrase_gen.h: No such file` (and `test_phrase_gen.cpp` not yet in the build).

- [ ] **Step 3: Create `engine/mod/phrase_gen.h` with the core**

```cpp
#pragma once
// Deterministic, allocation-free melodic phrase generator (PITCH lane).
// No heap, no virtual dispatch, no libDaisy. Every random draw goes through the
// caller's Rng so the engine's bit-determinism invariant holds.
#include <cstdint>
#include <cmath>
#include "mod/rng.h"

namespace spky {

enum class Principle : uint8_t {
    TwoMotif = 0, OneMotif, Hierarchical, CallResponse, Ostinato, kCount
};

// motif_count = number of RENEW renewal units (regenerate_unit's `unit` domain).
struct PhraseLayout {
    uint8_t motif_len   = 0;  // L: slots per instance
    uint8_t tail_len    = 0;  // r: trailing slots (0..L-1)
    uint8_t inst_count  = 0;  // k: number of instances
    uint8_t motif_count = 0;  // number of renewal units
};

inline float pg_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Metric weight of an absolute slot: downbeat strongest, then binary
// subdivisions (by trailing zeros of the slot index). Higher = stronger beat.
// Used for gate placement (motif-relative) and DENSITY (absolute).
inline float pg_metric_weight(int pos) {
    if (pos <= 0) return 1.0f;
    unsigned p = static_cast<unsigned>(pos);
    unsigned tz = 0;
    while ((p & 1u) == 0u && tz < 5u) { p >>= 1u; ++tz; }
    float w = 0.2f + 0.15f * static_cast<float>(tz);
    return w > 1.0f ? 1.0f : w;
}

// Bounded random walk: a line, not independent draws. Cubed step (small
// intervals common, leaps rare) with mild gravity pulling toward 0 (the root).
inline void pg_contour_walk(Rng& rng, float* out, int len,
                            float start, float width, float gravity) {
    float v = start;
    for (int i = 0; i < len; ++i) {
        float r = rng.next_bipolar();
        float step = r * r * r * width;
        v = pg_clampf((v + step) * (1.0f - gravity), -1.0f, 1.0f);
        out[i] = v;
    }
}

// Per-principle target motif length (slots). Tuned by ear; fixed (YAGNI).
inline int pg_target_len(Principle) { return 8; }

// Derive motif sizing from n (= min(steps,32)) and target length:
// k instances of length L, plus an r-slot tail. Invariant k*L + r == n.
inline void pg_derive_sizing(Principle p, int n, int& k, int& L, int& r) {
    int Lt = pg_target_len(p);
    if (n < 1) n = 1;
    k = static_cast<int>(std::lround(static_cast<float>(n) / static_cast<float>(Lt)));
    if (k < 1) k = 1;
    if (k > n) k = n;
    L = n / k;
    if (L < 1) { L = 1; k = n; }
    r = n - k * L;
}

// Fill per-instance motif id (which content block a slot copies; siblings share
// content) and unit id (which RENEW renewal unit an instance belongs to).
// Pure structure, no RNG. Ids are dense from 0. motif/unit counts reported.
inline void pg_build_arrangement(Principle p, int k,
                                 uint8_t* motif_of_inst, uint8_t* unit_of_inst,
                                 int& motif_count, int& unit_count) {
    if (k < 1) k = 1;
    switch (p) {
    case Principle::TwoMotif: {
        // A A B A rolled; degrades: k1->A, k2->A B, k3->A A B, k>=4->A..B A
        if (k == 1) { motif_of_inst[0] = 0; motif_count = 1; }
        else if (k == 2) { motif_of_inst[0] = 0; motif_of_inst[1] = 1; motif_count = 2; }
        else {
            for (int j = 0; j < k; ++j)
                motif_of_inst[j] = static_cast<uint8_t>((j == k - 2) ? 1 : 0);
            motif_count = 2;
        }
        for (int j = 0; j < k; ++j) unit_of_inst[j] = motif_of_inst[j];
        unit_count = motif_count;
        break;
    }
    case Principle::OneMotif:
    case Principle::Ostinato: {
        for (int j = 0; j < k; ++j) { motif_of_inst[j] = 0; unit_of_inst[j] = 0; }
        motif_count = unit_count = 1;
        break;
    }
    case Principle::Hierarchical: {
        // A B A B rolled; each motif is internally cell-tiled (nested repetition).
        for (int j = 0; j < k; ++j) {
            motif_of_inst[j] = static_cast<uint8_t>(j & 1);
            unit_of_inst[j]  = static_cast<uint8_t>(j & 1);
        }
        motif_count = unit_count = (k >= 2) ? 2 : 1;
        break;
    }
    case Principle::CallResponse: {
        // Q A Q A: even instance = question, odd = answer. A renewal unit is a
        // Q&A pair (regenerated together so the answer still resolves to root).
        for (int j = 0; j < k; ++j) {
            motif_of_inst[j] = static_cast<uint8_t>(j);      // each its own content
            unit_of_inst[j]  = static_cast<uint8_t>(j / 2);  // paired
        }
        motif_count = k;
        unit_count  = (k + 1) / 2;
        break;
    }
    default: {
        for (int j = 0; j < k; ++j) { motif_of_inst[j] = 0; unit_of_inst[j] = 0; }
        motif_count = unit_count = 1;
    }
    }
}

} // namespace spky
```

- [ ] **Step 4: Add the test to the build**

In `CMakeLists.txt`, find the `add_executable(spky_tests ...)` / target source list and add `tests/test_phrase_gen.cpp` alongside the other `tests/test_*.cpp` entries.

- [ ] **Step 5: Build and run only the new test**

```bash
source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (all, including the four new `test_phrase_gen` cases).

- [ ] **Step 6: Commit**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp CMakeLists.txt
git commit -m "feat(mod): phrase_gen core — Principle, PhraseLayout, sizing/arrangement helpers"
```

---

## Task 2: `generate_phrase` — motif content + full-phrase assembly

**Files:**
- Modify: `engine/mod/phrase_gen.h` (append `pg_gen_motif` + `generate_phrase` before `} // namespace spky`)
- Test: `tests/test_phrase_gen.cpp` (add cases)

**Interfaces:**
- Consumes: everything from Task 1.
- Produces: `void generate_phrase(Principle, Rng&, int steps, float* pitch, bool* gate, uint8_t* motif_id, PhraseLayout& out)` — fills `pitch[0..n)`, `gate[0..n)`, `motif_id[0..n)` for `n = min(steps,32)` and writes the layout. Deterministic per rng. Internal: `void pg_gen_motif(Principle, Rng&, int motif_id, int L, float* pitch, bool* gate)`.

- [ ] **Step 1: Write the failing test (add to `tests/test_phrase_gen.cpp`)**

```cpp
TEST_CASE("generate_phrase: TwoMotif shows motivic repetition, deterministic") {
    Rng a; a.seed(0xBEEF);
    Rng b; b.seed(0xBEEF);
    float pa[32], pb[32]; bool ga[32], gb[32]; uint8_t ma[32], mb[32];
    PhraseLayout la, lb;
    generate_phrase(Principle::TwoMotif, a, 32, pa, ga, ma, la);
    generate_phrase(Principle::TwoMotif, b, 32, pb, gb, mb, lb);
    CHECK(la.inst_count == 4);
    CHECK(la.motif_len == 8);
    // A A B A: slots 0-7 == 8-7 == 24-31 (id 0), 16-23 differ (id 1)
    for (int i = 0; i < 8; ++i) {
        CHECK(pa[i] == doctest::Approx(pa[8 + i]));
        CHECK(pa[i] == doctest::Approx(pa[24 + i]));
    }
    bool any_diff = false;
    for (int i = 0; i < 8; ++i) if (pa[i] != pa[16 + i]) any_diff = true;
    CHECK(any_diff);
    for (int i = 0; i < 32; ++i) CHECK(pa[i] == doctest::Approx(pb[i])); // determinism
    // has at least one rest somewhere (gate layer active)
    bool any_rest = false; for (int i = 0; i < 32; ++i) any_rest |= !ga[i];
    CHECK(any_rest);
}

TEST_CASE("generate_phrase: OneMotif repeats one identical motif") {
    Rng r; r.seed(7);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::OneMotif, r, 32, p, g, m, L);
    for (int i = 0; i < 8; ++i) {
        CHECK(p[i] == doctest::Approx(p[8 + i]));
        CHECK(p[i] == doctest::Approx(p[16 + i]));
        CHECK(p[i] == doctest::Approx(p[24 + i]));
    }
}

TEST_CASE("generate_phrase: Ostinato is near-static pitch, dense gate") {
    Rng r; r.seed(11);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::Ostinato, r, 32, p, g, m, L);
    float mn = 2.f, mx = -2.f;
    int on = 0;
    for (int i = 0; i < 32; ++i) { mn = std::min(mn, p[i]); mx = std::max(mx, p[i]); on += g[i]; }
    CHECK((mx - mn) < 0.35f);   // near-static
    CHECK(on >= 16);            // dense, at least half on
}

TEST_CASE("generate_phrase: CallResponse answer resolves to root") {
    Rng r; r.seed(13);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::CallResponse, r, 16, p, g, m, L); // k=2, L=8
    CHECK(p[15] == doctest::Approx(0.0f)); // answer's last slot lands on root
}

TEST_CASE("generate_phrase: Hierarchical tiles a cell inside each motif") {
    Rng r; r.seed(17);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::Hierarchical, r, 16, p, g, m, L); // k=2, L=8, cell=4
    CHECK(p[0] == doctest::Approx(p[4]));  // cell repeats within instance 0
    CHECK(p[1] == doctest::Approx(p[5]));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
source env.sh && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `generate_phrase` not declared.

- [ ] **Step 3: Append `pg_gen_motif` + `generate_phrase` to `phrase_gen.h`** (immediately before `} // namespace spky`)

```cpp
// Generate one motif's content (pitch + gate), length L. Gate uses motif-RELATIVE
// metric weight so sibling instances copy byte-identically; for aligned L this
// equals the absolute-slot weight (§spec). CallResponse role is by id parity.
inline void pg_gen_motif(Principle p, Rng& rng, int motif_id, int L,
                         float* pitch, bool* gate) {
    switch (p) {
    case Principle::Ostinato: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.05f, 0.30f); // near-static
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.30f;
        break;
    }
    case Principle::Hierarchical: {
        int cl = (L >= 6) ? 4 : 2;              // cell length 2 or 4
        if (cl > L) cl = L;
        float cell[4]; bool cellg[4];
        pg_contour_walk(rng, cell, cl, 0.0f, 0.6f, 0.10f);
        for (int i = 0; i < cl; ++i) cellg[i] = pg_metric_weight(i) >= 0.25f;
        for (int i = 0; i < L; ++i) { pitch[i] = cell[i % cl]; gate[i] = cellg[i % cl]; }
        break;
    }
    case Principle::CallResponse: {
        bool answer = (motif_id & 1) != 0;
        pg_contour_walk(rng, pitch, L, answer ? 0.5f : 0.0f, 0.6f, answer ? 0.25f : 0.05f);
        if (L > 0) {
            if (answer) pitch[L - 1] = 0.0f;                              // resolve
            else if (std::fabs(pitch[L - 1]) < 0.3f) pitch[L - 1] = 0.5f; // stay open
        }
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.25f;
        break;
    }
    case Principle::TwoMotif:
    case Principle::OneMotif:
    default: {
        pg_contour_walk(rng, pitch, L, 0.0f, 0.6f, 0.12f);
        for (int i = 0; i < L; ++i) gate[i] = pg_metric_weight(i) >= 0.25f;
        break;
    }
    }
}

// Fill pitch/gate/motif_id[0..n) for n = min(steps,32); write the layout.
// Deterministic per rng. RNG is consumed in ascending motif-id order, then tail.
inline void generate_phrase(Principle p, Rng& rng, int steps,
                            float* pitch, bool* gate, uint8_t* motif_id,
                            PhraseLayout& out) {
    int n = steps; if (n > 32) n = 32; if (n < 1) n = 1;
    int k, L, r;
    pg_derive_sizing(p, n, k, L, r);

    uint8_t moti[32], uniti[32];
    int motif_count, unit_count;
    pg_build_arrangement(p, k, moti, uniti, motif_count, unit_count);

    int n_ids = 1;
    for (int j = 0; j < k; ++j) if (moti[j] + 1 > n_ids) n_ids = moti[j] + 1;

    // Generate distinct content once per id (ascending), then scatter to instances.
    float cpitch[32]; bool cgate[32];              // n_ids*L <= n <= 32
    for (int id = 0; id < n_ids; ++id)
        pg_gen_motif(p, rng, id, L, cpitch + id * L, cgate + id * L);

    for (int j = 0; j < k; ++j) {
        int id = moti[j];
        for (int i = 0; i < L; ++i) {
            int slot = j * L + i;
            pitch[slot]    = cpitch[id * L + i];
            gate[slot]     = cgate[id * L + i];
            motif_id[slot] = static_cast<uint8_t>(id);
        }
    }

    if (r > 0) {                                    // tail motif, its own id
        float tp[32]; bool tg[32];
        pg_gen_motif(p, rng, n_ids, r, tp, tg);
        for (int i = 0; i < r; ++i) {
            int slot = k * L + i;
            pitch[slot]    = tp[i];
            gate[slot]     = tg[i];
            motif_id[slot] = static_cast<uint8_t>(n_ids);
        }
    }

    out.motif_len   = static_cast<uint8_t>(L);
    out.tail_len    = static_cast<uint8_t>(r);
    out.inst_count  = static_cast<uint8_t>(k);
    out.motif_count = static_cast<uint8_t>(unit_count);
}
```

- [ ] **Step 4: Build and run**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS (all `test_phrase_gen` cases).

- [ ] **Step 5: Commit**

```bash
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp
git commit -m "feat(mod): generate_phrase — motif content + five-principle assembly"
```

---

## Task 3: `regenerate_unit` — in-place per-unit RENEW

**Files:**
- Modify: `engine/mod/phrase_gen.h` (append `regenerate_unit`)
- Test: `tests/test_phrase_gen.cpp` (add cases)

**Interfaces:**
- Produces: `void regenerate_unit(Principle, Rng&, const PhraseLayout& layout, const uint8_t* motif_id, int unit, float* pitch, bool* gate)` — rewrites every slot of renewal `unit` (all sibling instances of its motif id(s)) in place; `unit` in `[0, layout.motif_count)`. Leaves arrangement, `motif_id[]`, tail and other units untouched. Deterministic per rng; gate placement matches `generate_phrase` (motif-relative), so regenerated rests match what `generate_phrase` would place at those slots.

- [ ] **Step 1: Write the failing test (add to `tests/test_phrase_gen.cpp`)**

```cpp
TEST_CASE("regenerate_unit: siblings stay identical, other units untouched") {
    Rng r; r.seed(0x1234);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::TwoMotif, r, 32, p, g, m, L); // A A B A, units {0,1}
    float b_id1[8]; for (int i = 0; i < 8; ++i) b_id1[i] = p[16 + i]; // motif B before
    Rng r2; r2.seed(0x55);
    regenerate_unit(Principle::TwoMotif, r2, L, m, /*unit=*/0, p, g);
    // all id-0 instances (0-7, 8-15, 24-31) still identical to each other
    for (int i = 0; i < 8; ++i) {
        CHECK(p[i] == doctest::Approx(p[8 + i]));
        CHECK(p[i] == doctest::Approx(p[24 + i]));
    }
    // unit 1 (motif B) unchanged
    for (int i = 0; i < 8; ++i) CHECK(p[16 + i] == doctest::Approx(b_id1[i]));
}

TEST_CASE("regenerate_unit: CallResponse pair regenerates, answer still resolves") {
    Rng r; r.seed(0x99);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::CallResponse, r, 16, p, g, m, L); // 1 unit (pair)
    Rng r2; r2.seed(0x2);
    regenerate_unit(Principle::CallResponse, r2, L, m, 0, p, g);
    CHECK(p[15] == doctest::Approx(0.0f)); // answer resolves after regen
}

TEST_CASE("regenerate_unit gates match generate_phrase at the same slots") {
    Rng r; r.seed(0x321);
    float p[32]; bool g[32]; uint8_t m[32]; PhraseLayout L;
    generate_phrase(Principle::TwoMotif, r, 32, p, g, m, L);
    Rng r2; r2.seed(0x321 ^ 0xABC);
    bool before[8]; for (int i = 0; i < 8; ++i) before[i] = g[16 + i];
    regenerate_unit(Principle::TwoMotif, r2, L, m, 1, p, g); // motif B
    // gate pattern of a regenerated motif follows the same metric rule => unchanged
    for (int i = 0; i < 8; ++i) CHECK(g[16 + i] == before[i]);
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
source env.sh && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `regenerate_unit` not declared.

- [ ] **Step 3: Append `regenerate_unit` to `phrase_gen.h`** (before `} // namespace spky`)

```cpp
// Regenerate renewal unit `unit` in place across ALL its slots (every sibling
// instance of its motif id(s); for CallResponse both the Q and A of the pair).
// Arrangement is deterministic and re-derived here, so motif_id[]/layout stay
// authoritative and untouched. RNG consumed in ascending motif-id order.
inline void regenerate_unit(Principle p, Rng& rng, const PhraseLayout& layout,
                            const uint8_t* /*motif_id*/, int unit,
                            float* pitch, bool* gate) {
    int k = layout.inst_count;
    int L = layout.motif_len;
    if (k < 1 || L < 1) return;

    uint8_t moti[32], uniti[32];
    int mc, uc;
    pg_build_arrangement(p, k, moti, uniti, mc, uc);

    // Which motif ids belong to this unit?
    bool idsel[32] = {};
    int maxid = 0;
    for (int j = 0; j < k; ++j) {
        if (uniti[j] == unit) idsel[moti[j]] = true;
        if (moti[j] > maxid) maxid = moti[j];
    }
    // Regenerate each selected id once (ascending), scatter to its instances.
    for (int id = 0; id <= maxid; ++id) {
        if (!idsel[id]) continue;
        float cp[32]; bool cg[32];
        pg_gen_motif(p, rng, id, L, cp, cg);
        for (int j = 0; j < k; ++j) {
            if (moti[j] != id) continue;
            for (int i = 0; i < L; ++i) {
                int slot = j * L + i;
                pitch[slot] = cp[i];
                gate[slot]  = cg[i];
            }
        }
    }
}
```

- [ ] **Step 4: Build and run**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add engine/mod/phrase_gen.h tests/test_phrase_gen.cpp
git commit -m "feat(mod): regenerate_unit — in-place per-unit RENEW regeneration"
```

---

## Task 4: Remove capture / replay from the whole stack

Capture is orthogonal to the melody rework and must go first so later `_on_boundary`/`process` edits are clean. This task only deletes; behaviour of entropy/probability is untouched.

**Files:**
- Delete: `engine/mod/capture.h`, `tests/test_capture.cpp`
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`, `engine/mod/super_modulator.h`, `engine/mod/super_modulator.cpp`, `engine/instrument.h`, `host/render/scenario.cpp`, `host/render/main.cpp`, `CMakeLists.txt`, `tests/test_super_modulator.cpp`, `tests/test_instrument.cpp`, `tests/test_scenario.cpp`

- [ ] **Step 1: Delete capture files and remove from build**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git rm engine/mod/capture.h tests/test_capture.cpp
```
In `CMakeLists.txt`, remove `tests/test_capture.cpp` from the `spky_tests` source list.

- [ ] **Step 2: Strip capture from `engine/mod/lane.h`**

Remove: the `class CaptureLoop;` forward decl (line ~8); `set_capture_loop`, `set_replay`, `replaying` methods (lines ~35–37); private `_record_slot`, `_replaying`, `_replay_step`, `_phase_slot` decls (lines ~51–53, 50); fields `_capture_loop`, `_rec_slot`, `_rec_fired`, `_replay`, `_play_slot` (lines ~90–94). Leave everything else.

- [ ] **Step 3: Strip capture from `engine/mod/lane.cpp`**

- In `kick` (lines ~67–72): remove the `if (_replaying()) return;` guard line.
- Delete the methods `_phase_slot`, `_record_slot`, `_replaying`, `_replay_step` (lines ~128–155).
- In `process` (lines ~157–206): remove `const bool replay = _replaying();`; change the phase advance back to unconditional `_phase += _phase_inc * (1.f + _ev_rate);`; delete the `if (replay) { _replay_step(); } else {` wrapper so the `else` body runs unconditionally (keep its contents — wrap handling, step/flow boundary, `_target` update); delete the trailing `if (_capture_loop) _record_slot();` line.

- [ ] **Step 4: Strip capture from `super_modulator.{h,cpp}`**

- `super_modulator.h`: remove `capture_now`, `set_replay`, `replaying`, `loop_valid` (lines ~45–48) and the `CaptureLoop _capture;` member (line ~56). Remove any `#include "mod/capture.h"`.
- `super_modulator.cpp`: in `init` remove `_capture.reset();` and `_lanes[LANE_PITCH].set_capture_loop(&_capture);` (lines ~31–32).

- [ ] **Step 5: Strip capture from `engine/instrument.h`**

Remove `capture_now(int)`, `set_replay(int,bool)`, `replaying(int)`, `loop_valid(int)` (lines ~86–89). Remove any `#include "mod/capture.h"`.

- [ ] **Step 6: Strip capture from `host/render/scenario.cpp`**

Remove the `capture_now` and `set_replay` action branches (lines ~119–120) from `apply_event`.

- [ ] **Step 7: Strip capture columns from `host/render/main.cpp`**

Remove the `a_cap` and `b_cap` CSV header entries (line ~44/47) and the two `replaying(p) ? 1 : 0` value writes (line ~76). Adjust the header string so the column count matches the values written.

- [ ] **Step 8: Remove capture cases from tests**

- `tests/test_super_modulator.cpp`: delete any TEST_CASE referencing `loop_valid`/`set_replay`/capture.
- `tests/test_instrument.cpp`: delete any TEST_CASE referencing `capture_now`/`set_replay`/`replaying`/`loop_valid`.
- `tests/test_scenario.cpp`: delete the case dispatching `capture_now`/`set_replay`.

- [ ] **Step 9: Build and run**

```bash
source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS. If a lingering reference to a removed symbol breaks the build, grep and remove it:
```bash
grep -rn "capture\|replay\|CaptureLoop\|loop_valid" engine host tests | grep -v Binary
```
Expected after fixes: no matches in code (comments referencing the removed feature are fine to delete too).

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "refactor: remove capture/replay sequencer from engine, host and tests"
```

---

## Task 5: Melody data model + PhraseGen-seeded init + principle plumbing

Add the melody state and seed the buffer from the generator. Gates exist but are not yet consulted by `_on_boundary` (still probability-driven) — that lands in Task 6.

**Files:**
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`, `engine/mod/super_modulator.{h,cpp}`, `engine/instrument.h`, `host/render/scenario.cpp`
- Test: `tests/test_variation.cpp` (**new**, replaces `test_entropy_seq.cpp`), `CMakeLists.txt`
- Delete: `tests/test_entropy_seq.cpp`

**Interfaces:**
- Produces on `ModLane`: `void set_melodic(bool)`, `void set_principle(Principle)`; fields `bool _gate[kSeqSlots]`, `uint8_t _motif_id[kSeqSlots]`, `PhraseLayout _layout`, `Principle _principle`, `bool _melodic`, `float _density`, `bool _regen_pending`; private `void _fill_walk()`.
- On `SuperModulator`: `void set_principle(Principle)`. On `Instrument`: `void set_principle(int part, int principle)`.

- [ ] **Step 1: Write the failing test**

Delete the old file and create `tests/test_variation.cpp`. This file's shared harness (a `collect_cycles` helper keyed on **phase-wrap events**, not sample counts, so it is immune to float phase-accumulation jitter) is reused and extended in Tasks 7, 8 and 11:
```bash
git rm tests/test_entropy_seq.cpp
```
```cpp
#include <doctest/doctest.h>
#include "mod/lane.h"
#include <cmath>
#include <vector>

using namespace spky;

// Build a melodic (PITCH-style) STEP lane at pure S&H (shape 1.0).
// NOTE: `set_variation` is added in Task 7; until then this helper calls
// `set_entropy(0.f)`. Switch the one call to `set_variation` in Task 7.
static ModLane make_melodic_step_lane(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(8.0f);
    l.set_entropy(0.f);        // -> set_variation(0.f) in Task 7
    return l;
}

// Drive the lane and return raw process() outputs.
static std::vector<float> drive(ModLane& l, int samples) {
    std::vector<float> out;
    for (int n = 0; n < samples; ++n) out.push_back(l.process());
    return out;
}

// Collect fired-note target() values grouped by cycle, split on phase wrap
// (phase() decreasing) so grouping is exact regardless of sample timing.
static std::vector<std::vector<float>> collect_cycles(ModLane& l, int cycles) {
    std::vector<std::vector<float>> out(1);
    float prev = l.phase();
    int wraps = 0;
    for (int n = 0; n < 300000 && wraps <= cycles; ++n) {
        l.process();
        float ph = l.phase();
        if (ph < prev) { out.emplace_back(); ++wraps; } // new cycle starts here
        prev = ph;
        if (l.fired()) out.back().push_back(l.target());
    }
    return out;
}

TEST_CASE("melodic init is deterministic per seed") {
    ModLane a = make_melodic_step_lane(0xABCDEF, 32);
    ModLane b = make_melodic_step_lane(0xABCDEF, 32);
    auto oa = drive(a, 12000), ob = drive(b, 12000);
    REQUIRE(oa.size() == ob.size());
    for (size_t i = 0; i < oa.size(); ++i) CHECK(oa[i] == ob[i]); // bit-identical
}

TEST_CASE("distinct seeds give distinct melodies") {
    ModLane a = make_melodic_step_lane(0x111, 32);
    ModLane b = make_melodic_step_lane(0x222, 32);
    auto oa = drive(a, 12000), ob = drive(b, 12000);
    bool differ = false;
    for (size_t i = 0; i < oa.size(); ++i) if (oa[i] != ob[i]) differ = true;
    CHECK(differ);
}
```
> The deep structural assertions (motivic repetition, principle signatures) live in `test_phrase_gen.cpp`; this file owns lane-level LOOP/GROW/RENEW behaviour, filled in over Tasks 5–8.

- [ ] **Step 2: Run to verify it fails**

```bash
source env.sh && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `set_melodic`/`set_principle` not declared on `ModLane` (the helper deliberately still calls `set_entropy`, which exists until Task 7).

- [ ] **Step 3: Add fields + methods to `lane.h`**

Add `#include "mod/phrase_gen.h"` near the top. In the class add public:
```cpp
    void set_melodic(bool m) { _melodic = m; }
    void set_principle(Principle p) { _principle = p; }
```
Private decl: `void _fill_walk();`
Fields (near `_seq`):
```cpp
    bool      _gate[kSeqSlots]      = {};
    uint8_t   _motif_id[kSeqSlots]  = {};
    PhraseLayout _layout;
    Principle _principle = Principle::TwoMotif;
    bool      _melodic   = false;
    float     _density   = 1.f;
    bool      _regen_pending = false;
```

- [ ] **Step 4: Seed the melody in `lane.cpp init`**

Add `#include "mod/phrase_gen.h"` if not already via the header. Replace the per-slot prefill loop (`for (float& v : _seq) v = _rng.next_bipolar();`, line ~20) with:
```cpp
    if (_melodic) {
        generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);
    } else {
        _fill_walk();
        for (int i = 0; i < kSeqSlots; ++i) { _gate[i] = true; _motif_id[i] = 0; }
    }
    _regen_pending = false;
    _density = 1.f;
```
Add the helper (deterministic line, fixes salad on all lanes):
```cpp
void ModLane::_fill_walk() {
    pg_contour_walk(_rng, _seq, kSeqSlots, 0.f, 0.6f, 0.12f);
}
```

- [ ] **Step 5: Plumb `set_melodic` + `set_principle` through super_modulator**

`super_modulator.cpp init` — set melodic **before** each lane inits:
```cpp
    for (int i = 0; i < LANE_COUNT; ++i) {
        _lanes[i].set_melodic(i == LANE_PITCH);
        _lanes[i].init(sample_rate, seed_base + i * 2654435761u);
    }
```
`super_modulator.h` add:
```cpp
    void set_principle(Principle p) { _lanes[LANE_PITCH].set_principle(p); }
```
`instrument.h` add:
```cpp
    void set_principle(int p, int pr) { _parts[p].mod().set_principle(static_cast<Principle>(pr)); }
```
(`Principle` is visible via `super_modulator.h` → `phrase_gen.h`.)

- [ ] **Step 6: Add the scenario action**

`host/render/scenario.cpp apply_event` — add:
```cpp
    else if (e.action == "set_principle") inst.set_principle(e.part, e.ivalue);
```

- [ ] **Step 7: Add the test file to the build, build and run**

Add `tests/test_variation.cpp` to `CMakeLists.txt` (and confirm `test_entropy_seq.cpp` is removed from it).
```bash
source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(mod): melodic data model + PhraseGen-seeded init + principle plumbing"
```

---

## Task 6: Gate layer + DENSITY + remove PROBABILITY

**Files:**
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`, `engine/mod/super_modulator.{h,cpp}`, `engine/instrument.h`, `host/render/scenario.cpp`, `tests/test_lane.cpp`
- Test: `tests/test_gate_density.cpp` (**new**), `CMakeLists.txt`

**Interfaces:**
- Produces on `ModLane`: `void set_density(float)`; private `bool _effective_gate(int) const`, `bool _density_pass(int) const`. Removes `set_probability`, `_prob`.
- On `SuperModulator`/`Instrument`: `set_density` forwarded to PITCH.

- [ ] **Step 1: Write the failing test**

Create `tests/test_gate_density.cpp`. It deliberately uses **no** variation setter (the default is 0 / LOOP), so it survives the Task 7 rename untouched:
```cpp
#include <doctest/doctest.h>
#include "mod/lane.h"

using namespace spky;

static ModLane melodic_step(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);           // variation defaults to 0 (LOOP)
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_rate_hz(2.0f);
    return l;
}

// Count fired notes over a fixed span (many cycles); at variation 0 the rate is
// constant so full-vs-thin spans are directly comparable.
static int count_fires(ModLane& l, int samples) {
    int fires = 0;
    for (int n = 0; n < samples; ++n) { l.process(); if (l.fired()) ++fires; }
    return fires;
}

TEST_CASE("DENSITY low drops weak-beat gates; downbeat survives") {
    ModLane full = melodic_step(0x11, 16);
    ModLane thin = melodic_step(0x11, 16);
    thin.set_density(0.2f);
    int f_full = count_fires(full, 24000);
    int f_thin = count_fires(thin, 24000);
    CHECK(f_thin < f_full);          // fewer notes fire when thinned
    CHECK(f_thin >= 1);              // strong beats still fire
}

TEST_CASE("DENSITY is reversible: density 1 == the full pattern") {
    ModLane a = melodic_step(0x11, 16);
    ModLane b = melodic_step(0x11, 16);
    b.set_density(0.2f);             // thin...
    b.set_density(1.0f);             // ...then restore (never edits _gate)
    CHECK(count_fires(a, 24000) == count_fires(b, 24000));
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

- [ ] **Step 2: Run to verify it fails**

Add `tests/test_gate_density.cpp` to the `spky_tests` list in `CMakeLists.txt` first so it compiles, then:
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `set_density` not declared on `ModLane`. (The test uses no variation setter, so the Task 7 rename does not touch it.)

- [ ] **Step 3: Add gate/density logic to `lane.h`**

Public: `void set_density(float d) { _density = pg_clampf(d, 0.f, 1.f); }`
Remove `void set_probability(float p);` decl and the `float _prob = 1.f;` field.
Private decls:
```cpp
    bool _effective_gate(int slot) const;
    bool _density_pass(int slot) const;
```

- [ ] **Step 4: Implement gate/density + rewire `_on_boundary` in `lane.cpp`**

Remove `void ModLane::set_probability(float p) { _prob = clampf(p,0.f,1.f); }` (line ~40s).
Add:
```cpp
bool ModLane::_density_pass(int slot) const {
    // density 1 -> threshold 0 (all pass); density 0 -> threshold 1 (only slot 0).
    return pg_metric_weight(slot) >= (1.f - _density);
}
bool ModLane::_effective_gate(int slot) const {
    return _gate[slot] && _density_pass(slot);
}
```
Rewrite `_on_boundary` (was lines ~97–106):
```cpp
void ModLane::_on_boundary() {
    int slot = _sh_slot();
    // STEP consults the effective gate (note/rest + density mask); FLOW has no
    // per-step gate so it always fires (no freeze source after PROBABILITY).
    bool gated = _step_mode ? _effective_gate(slot) : true;
    _frozen = !gated;
    if (gated) {
        _fired = true;
        if (_entropy > 0.f) _mutate_slot(slot);  // GROW pitch; renamed in Task 7
        _target = _compute_raw();
    }
}
```
> `_mutate_slot` is still the entropy version here (grow + erode); Task 7 restricts it to pitch-only GROW and removes erode. The `_entropy > 0.f` guard already limits mutation to GROW at boundaries.

- [ ] **Step 5: Remove PROBABILITY from super_modulator / instrument / scenario; add set_density**

- `super_modulator.h`: remove `set_probability` fan-out; add `void set_density(float d) { _lanes[LANE_PITCH].set_density(d); }`.
- `super_modulator.cpp`: remove the `set_probability` fan-out definition if present.
- `instrument.h`: remove `set_probability(int,float)`; add `void set_density(int p, float d) { _parts[p].mod().set_density(d); }`.
- `scenario.cpp apply_event`: remove the `set_probability` branch; add `else if (e.action == "set_density") inst.set_density(e.part, e.value);`.

- [ ] **Step 6: Remove the probability case from `tests/test_lane.cpp`**

Delete the "prob-0 freeze" TEST_CASE (it exercised `set_probability`). Keep rate/range/smooth/kick/shape_offset cases.

- [ ] **Step 7: Build and run**

```bash
source env.sh && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
grep -rn "set_probability\|_prob\b" engine host tests
```
Expected: PASS; grep returns nothing.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(mod): gate/rest + effective-gate DENSITY; remove PROBABILITY"
```

---

## Task 7: Rename entropy→variation + GROW pitch-only + RENEW mechanics

**Files:**
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`, `engine/mod/super_modulator.{h,cpp}`, `engine/instrument.h`, `host/render/scenario.cpp`, `tests/test_variation.cpp`

**Interfaces:**
- Produces on `ModLane`: `void set_variation(float)` (replaces `set_entropy`); private `void _renew_units()`, `void _renew_walk()`; field `float _variation` (replaces `_entropy`). `_mutate_slot` becomes GROW-only (pitch), no erode.

- [ ] **Step 1: Update the RENEW/GROW test (`tests/test_variation.cpp`)**

First, switch the `make_melodic_step_lane` helper's one `set_entropy(0.f)` call to `set_variation(0.f)` (the rename lands in this task). Then add these behaviour cases (they reuse the `collect_cycles` / `drive` helpers already in the file):
```cpp
// variation-parameterised builder layered on the file's base helper.
static ModLane melodic_var(uint32_t seed, int steps, float variation) {
    ModLane l = make_melodic_step_lane(seed, steps);
    l.set_variation(variation);
    return l;
}

TEST_CASE("variation 0: consecutive cycles identical (LOOP), pitch and rhythm") {
    ModLane l = melodic_var(0x100, 16, 0.f);
    auto cy = collect_cycles(l, 4);
    REQUIRE(cy.size() >= 4);
    // compare two mid cycles (avoid the init edge); identical at LOOP.
    REQUIRE(cy[1].size() == cy[2].size());
    for (size_t i = 0; i < cy[1].size(); ++i) CHECK(cy[1][i] == doctest::Approx(cy[2][i]));
}

TEST_CASE("GROW varies pitch within a cycle but keeps the same gate rhythm") {
    ModLane loop = melodic_var(0x200, 16, 0.0f);
    ModLane grow = melodic_var(0x200, 16, 0.8f);
    auto cl = collect_cycles(loop, 2);
    auto cg = collect_cycles(grow, 2);
    // First full cycle (index 1): ev_rate is still ~0 so timing is identical =>
    // same number of notes fire (gates untouched by GROW).
    REQUIRE(cl.size() >= 2); REQUIRE(cg.size() >= 2);
    CHECK(cl[1].size() == cg[1].size());
    bool pitch_changed = false;
    for (size_t i = 0; i < cl[1].size(); ++i)
        if (std::fabs(cl[1][i] - cg[1][i]) > 0.01f) pitch_changed = true;
    CHECK(pitch_changed); // pitch drifted where GROW mutated fired slots
}

TEST_CASE("RENEW at -1 replaces units every cycle; still coherent") {
    ModLane l = melodic_var(0x300, 16, -1.0f);
    auto cy = collect_cycles(l, 4);
    REQUIRE(cy.size() >= 4);
    REQUIRE(cy[1].size() >= 4); REQUIRE(cy[2].size() >= 4);
    bool changed = false;
    size_t m = std::min(cy[1].size(), cy[2].size());
    for (size_t i = 0; i < m; ++i) if (std::fabs(cy[1][i] - cy[2][i]) > 0.01f) changed = true;
    CHECK(changed); // a new phrase per cycle at sustained -1
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
source env.sh && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `set_variation` not declared / `target()` mismatch.

- [ ] **Step 3: Rename in `lane.h`**

Replace `float _entropy = 0.f;` with `float _variation = 0.f;`. Replace `void set_entropy(float e);` decl with `void set_variation(float v);`. Add private `void _renew_units();` and `void _renew_walk();`.

- [ ] **Step 4: Rename + rework in `lane.cpp`**

- Replace `void ModLane::set_entropy(float e) { _entropy = clampf(e,-1.f,1.f); }` with `void ModLane::set_variation(float v) { _variation = clampf(v,-1.f,1.f); }`.
- Everywhere `_entropy` is read (in `_on_boundary`, `_mutate_slot`, `process` wrap), rename to `_variation`.
- Rewrite `_mutate_slot` to GROW-only (delete the erode branch and the `kErode`/`kRootSnap` constants at lines ~12–13):
```cpp
void ModLane::_mutate_slot(int slot) {
    if (_rng.next_unipolar() >= _variation * _variation) return; // dice ∝ variation²
    float v = _seq[slot];
    float r = _rng.next_bipolar();
    float delta = r * r * r * lerpf(0.5f, 2.f, _variation); // cubed: small common
    v = clampf((v + delta) * (1.f - kGravity), -1.f, 1.f);   // mild tonic gravity
    _seq[slot] = v;
}
```
- In `process`, rewrite the `if (wrapped) { ... }` block that handled entropy EVOLVE / erode (lines ~171–184) to:
```cpp
        if (wrapped) {
            if (_variation > 0.f) {                 // GROW: EVOLVE contour walk (live)
                _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _variation, -0.5f, 0.5f);
                _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _variation, -0.25f, 0.25f);
                _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _variation, -0.2f, 0.2f);
            } else if (_variation < 0.f) {          // RENEW: per-unit regen + walk decay
                if (_melodic && _step_mode) _renew_units();
                else if (!_melodic) {
                    if (_rng.next_unipolar() < _variation * _variation) _renew_walk();
                }
                float decay = 1.f + 0.2f * _variation;  // variation -1 -> x0.8/cycle
                _ev_phase *= decay; _ev_shape *= decay; _ev_rate *= decay;
            }                                       // variation 0 (LOOP): walk frozen
        }
```
- Add the RENEW helpers:
```cpp
void ModLane::_renew_units() {
    int units = _layout.motif_count;                 // number of renewal units
    for (int u = 0; u < units; ++u) {
        if (_rng.next_unipolar() < _variation * _variation)  // per-unit dice
            regenerate_unit(_principle, _rng, _layout, _motif_id, u, _seq, _gate);
    }
}
void ModLane::_renew_walk() {
    pg_contour_walk(_rng, _seq, kSeqSlots, 0.f, 0.6f, 0.12f);
}
```
- In `_on_boundary`, the GROW guard is now `if (_variation > 0.f) _mutate_slot(slot);` (already renamed in Step 4 bullet 2).

- [ ] **Step 5: Rename through super_modulator / instrument / scenario**

- `super_modulator.h`: `set_entropy` fan-out → `void set_variation(float v) { for (auto& l : _lanes) l.set_variation(v); }`.
- `super_modulator.cpp`: rename the fan-out definition if present.
- `instrument.h`: `set_entropy(int,float)` → `void set_variation(int p, float v) { _parts[p].mod().set_variation(v); }`.
- `scenario.cpp apply_event`: change the `set_entropy` branch to accept **both** names for a smooth scenario migration:
```cpp
    else if (e.action == "set_entropy" || e.action == "set_variation")
        inst.set_variation(e.part, e.value);
```

- [ ] **Step 6: Build and run**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
grep -rn "_entropy\|set_entropy\b\|kErode\|kRootSnap" engine
```
Expected: PASS; grep returns no engine hits (scenario keeps the `"set_entropy"` string alias only).

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(mod): set_variation — GROW pitch-only + per-unit RENEW; drop erode"
```

---

## Task 8: Live step-count change + new-phrase gesture

**Files:**
- Modify: `engine/mod/lane.h`, `engine/mod/lane.cpp`, `engine/mod/super_modulator.{h,cpp}`, `engine/instrument.h`, `host/render/scenario.cpp`
- Test: `tests/test_new_phrase.cpp` (**new**), `CMakeLists.txt`

**Interfaces:**
- Produces on `ModLane`: `void new_phrase()`; `set_step` sets `_regen_pending` **only when `min(_steps,32)` changes** (melodic); a pending regen runs `generate_phrase` at the next STEP-mode cycle wrap. On `SuperModulator`/`Instrument`: `new_phrase` forwarded to PITCH.

- [ ] **Step 1: Write the failing test**

Create `tests/test_new_phrase.cpp` (self-contained: its own wrap-based cycle collector, immune to sample-count drift):
```cpp
#include <doctest/doctest.h>
#include "mod/lane.h"
#include <vector>
#include <cmath>

using namespace spky;

static ModLane melodic(uint32_t seed, int steps) {
    ModLane l;
    l.set_melodic(true);
    l.set_principle(Principle::TwoMotif);
    l.init(48000.f, seed);
    l.set_shape(1.0f);
    l.set_step(true, steps);
    l.set_variation(0.f);
    l.set_rate_hz(8.0f);
    return l;
}

// Fired targets grouped by cycle, split on phase-wrap events.
static std::vector<std::vector<float>> cycles(ModLane& l, int n) {
    std::vector<std::vector<float>> out(1);
    float prev = l.phase();
    int wraps = 0;
    for (int s = 0; s < 300000 && wraps <= n; ++s) {
        l.process();
        float ph = l.phase();
        if (ph < prev) { out.emplace_back(); ++wraps; }
        prev = ph;
        if (l.fired()) out.back().push_back(l.target());
    }
    return out;
}

TEST_CASE("new_phrase regenerates at a cycle wrap, then loops") {
    ModLane l = melodic(0x400, 16);
    auto before = cycles(l, 2);          // establish the pre-regen phrase
    l.new_phrase();                      // pending; applies at the next wrap
    auto after = cycles(l, 3);
    REQUIRE(before.size() >= 2); REQUIRE(after.size() >= 3);
    // once regenerated, consecutive cycles are stable (LOOP)
    REQUIRE(after[1].size() == after[2].size());
    for (size_t i = 0; i < after[1].size(); ++i)
        CHECK(after[1][i] == doctest::Approx(after[2][i]));
    // and the phrase actually changed vs before the gesture
    bool changed = before[1].size() != after[2].size();
    if (!changed) for (size_t i = 0; i < before[1].size(); ++i)
        if (std::fabs(before[1][i] - after[2][i]) > 0.01f) changed = true;
    CHECK(changed);
}

TEST_CASE("set_step with unchanged effective length does not regen") {
    ModLane l = melodic(0x500, 16);
    auto c1 = cycles(l, 2);
    l.set_step(true, 16);                // same count -> no regen
    auto c2 = cycles(l, 2);
    REQUIRE(c1.size() >= 2); REQUIRE(c2.size() >= 2);
    REQUIRE(c1[1].size() == c2[1].size());
    for (size_t i = 0; i < c1[1].size(); ++i) CHECK(c1[1][i] == doctest::Approx(c2[1][i]));
}

TEST_CASE("moves within steps>=32 do not regen (n stays 32)") {
    ModLane l = melodic(0x600, 33);
    auto c1 = cycles(l, 2);
    l.set_step(true, 40);                // 33 -> 40, effective n stays 32 -> no regen
    auto c2 = cycles(l, 2);
    REQUIRE(c1.size() >= 2); REQUIRE(c2.size() >= 2);
    REQUIRE(c1[1].size() == c2[1].size());
    for (size_t i = 0; i < c1[1].size(); ++i) CHECK(c1[1][i] == doctest::Approx(c2[1][i]));
}
```

- [ ] **Step 2: Add to build; run to verify it fails**

Add `tests/test_new_phrase.cpp` to `CMakeLists.txt`.
```bash
source env.sh && cmake -S . -B build && cmake --build build 2>&1 | head -20
```
Expected: FAIL — `new_phrase` not declared.

- [ ] **Step 3: Add `new_phrase` + regen-on-step-change to `lane.h`/`lane.cpp`**

`lane.h` public: `void new_phrase();`
`lane.cpp` — rewrite `set_step`:
```cpp
void ModLane::set_step(bool on, int steps) {
    _step_mode = on;
    int new_steps = steps < 1 ? 1 : steps;
    if (_melodic) {
        int old_n = _steps > kSeqSlots ? kSeqSlots : _steps;
        int new_n = new_steps > kSeqSlots ? kSeqSlots : new_steps;
        if (new_n != old_n) _regen_pending = true; // only when effective length changes
    }
    _steps = new_steps;
}
```
Add:
```cpp
void ModLane::new_phrase() { if (_melodic) _regen_pending = true; }
```
In `process`, at the top of the `if (wrapped)` block (before the GROW/RENEW branch), add the pending full-regen — STEP + melodic only, so FLOW leaves it pending and a running bar finishes intact:
```cpp
            if (_regen_pending && _melodic && _step_mode) {
                generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);
                _regen_pending = false;
                _ev_phase = _ev_shape = _ev_rate = 0.f; // present fresh phrase un-warped
            }
```

- [ ] **Step 4: Plumb `new_phrase` through super_modulator / instrument / scenario**

- `super_modulator.h`: `void new_phrase() { _lanes[LANE_PITCH].new_phrase(); }`
- `instrument.h`: `void new_phrase(int p) { _parts[p].mod().new_phrase(); }`
- `scenario.cpp apply_event`: `else if (e.action == "new_phrase") inst.new_phrase(e.part);`

- [ ] **Step 5: Build and run**

```bash
source env.sh && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat(mod): live step-change regen + new_phrase gesture at cycle wrap"
```

---

## Task 9: Host — PITCH gate CSV column + scenario updates

**Files:**
- Modify: `engine/mod/lane.h`, `engine/mod/super_modulator.h`, `engine/instrument.h`, `host/render/main.cpp`
- Scenarios: retire `host/render/scenarios/capture_loop.json`, `capture_duet.json`, `capture_pentatonic.json`; rework `demo_step_melody.json`; add `host/render/scenarios/demo_density_sweep.json`

**Interfaces:**
- Produces: `bool ModLane::gate_state() const` → `SuperModulator::pitch_gate()` → `Instrument::pitch_gate(int)`; CSV columns `a_pgate` / `b_pgate` (1 = note, 0 = rest).

- [ ] **Step 1: Add a gate accessor down the stack**

- `lane.h` public: `bool gate_state() const { return _step_mode ? _effective_gate(_sh_slot()) : true; }` (place after `frozen()`; `_effective_gate` is already a const method).
- `super_modulator.h`: `bool pitch_gate() const { return _lanes[LANE_PITCH].gate_state(); }`
- `instrument.h`: `bool pitch_gate(int p) const { return _parts[p].mod().pitch_gate(); }`

- [ ] **Step 2: Add the CSV columns in `main.cpp`**

In the CSV header (near the removed `a_cap`), add `a_pgate` for part A and `b_pgate` for part B in the same per-part position the old `a_cap`/`b_cap` held. In the value row, write `inst.pitch_gate(p) ? 1 : 0` for each part at that column. Ensure header labels and value order line up.

- [ ] **Step 3: Rebuild and smoke-render**

```bash
source env.sh && cmake --build build
./build/render.exe host/render/scenarios/demo_step_melody.json out.wav mods.csv || true
head -1 mods.csv
```
Expected: build OK; `mods.csv` header contains `a_pgate`/`b_pgate` and no `a_cap`/`b_cap`. (The scenario is reworked next; a stale one may warn but should still render.)

- [ ] **Step 4: Retire capture scenarios**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
git rm host/render/scenarios/capture_loop.json host/render/scenarios/capture_duet.json host/render/scenarios/capture_pentatonic.json
```

- [ ] **Step 5: Rework `demo_step_melody.json` into a principle/variation showcase**

Overwrite `host/render/scenarios/demo_step_melody.json` (keep the existing engine/tone and top-level fields; only the `events`/`init` change). Structure: 16-step STEP PITCH melody, sweep MELODY right (GROW), back to LOOP, sweep left (RENEW), tap `new_phrase`, cycle a principle:
```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 40,
  "init": [
    { "action": "set_sync_mode", "part": 0, "ivalue": 0 },
    { "action": "set_step", "part": 0, "flag": true, "ivalue": 16 },
    { "action": "set_shape", "part": 0, "value": 1.0 },
    { "action": "set_principle", "part": 0, "ivalue": 0 },
    { "action": "set_variation", "part": 0, "value": 0.0 }
  ],
  "events": [
    { "t": 6,  "action": "set_variation", "part": 0, "value": 0.6 },
    { "t": 14, "action": "set_variation", "part": 0, "value": 0.0 },
    { "t": 20, "action": "set_variation", "part": 0, "value": -0.9 },
    { "t": 28, "action": "set_variation", "part": 0, "value": 0.0 },
    { "t": 30, "action": "new_phrase", "part": 0 },
    { "t": 34, "action": "set_principle", "part": 0, "ivalue": 4 },
    { "t": 35, "action": "new_phrase", "part": 0 }
  ]
}
```
> Verify field names (`t`, `action`, `part`, `flag`, `ivalue`, `value`) against `host/render/scenario.cpp::parse_event`; match the existing `demo_step_melody.json` engine/tone keys exactly (do not drop an engine selector the harness needs).

- [ ] **Step 6: Add `demo_density_sweep.json`**

Create `host/render/scenarios/demo_density_sweep.json`: a static Two-motif phrase with DENSITY swept full → strong-beats-only → back:
```json
{
  "sample_rate": 48000,
  "bpm": 120,
  "duration_s": 24,
  "init": [
    { "action": "set_step", "part": 0, "flag": true, "ivalue": 16 },
    { "action": "set_shape", "part": 0, "value": 1.0 },
    { "action": "set_principle", "part": 0, "ivalue": 0 },
    { "action": "set_variation", "part": 0, "value": 0.0 },
    { "action": "set_density", "part": 0, "value": 1.0 }
  ],
  "events": [
    { "t": 8,  "action": "set_density", "part": 0, "value": 0.2 },
    { "t": 16, "action": "set_density", "part": 0, "value": 1.0 }
  ]
}
```

- [ ] **Step 7: Render both demos to confirm they run**

```bash
source env.sh && cmake --build build
./build/render.exe host/render/scenarios/demo_step_melody.json /tmp/dsm.wav /tmp/dsm.csv
./build/render.exe host/render/scenarios/demo_density_sweep.json /tmp/dds.wav /tmp/dds.csv
```
Expected: both produce WAV + CSV with no error. Eyeball `dds.csv` `a_pgate` column: fewer 1s in the middle (t≈8–16), full again after.

- [ ] **Step 8: Full test run + commit**

```bash
ctest --test-dir build --output-on-failure
git add -A
git commit -m "feat(host): PITCH gate CSV column; retire capture demos; principle/variation + density showcase scenarios"
```

---

## Task 10: Master-spec touch-ups (residency repo)

**Files:**
- Modify (residency repo): `C:\Users\bernd\Documents\AI\Synthux Design Residency\docs\superpowers\specs\2026-07-10-spotykach-modulation-first-synth-design.md`

This is a documentation edit in the **residency** repo (not the fork). Apply the changes listed in the design spec's "Master-spec touch-ups" section.

- [ ] **Step 1: Read the master spec and locate the sections**

Read the file; find: the PROBABILITY knob on the macro surface and lane signal chain; the Capture-sequencer section; the ENTROPY paragraph; the Panel switches table (switch 2); the LED section.

- [ ] **Step 2: Apply the edits**

- Remove the PROBABILITY knob from the macro surface and the lane signal chain; change the PITCH chain `[wavetable]→[probability]→…` to `[wavetable]→[gate]→…`; rename CTRL_POS to **MELODY**.
- Remove the entire Capture-sequencer section; note ALT+SEQ now carries the principle/new-phrase gestures; remove the ring's step-pattern+playhead display line.
- Replace the ENTROPY paragraph with MELODY/VARIATION: bipolar RENEW / LOOP / GROW; PITCH gets the phrase generator + gate layer + density; the other four lanes get the plain LOOP/GROW/RENEW process.
- Panel switches table: switch 2 = free/reserve (was ERODE/LOOP/GROW).
- LED section: ring shows PITCH post-gate/post-density (frozen/rest dims as before); add the principle-overlay-on-SEQ-tap note; drop the capture display line.

- [ ] **Step 3: Commit (residency repo)**

```bash
cd "/c/Users/bernd/Documents/AI/Synthux Design Residency"
git add docs/superpowers/specs/2026-07-10-spotykach-modulation-first-synth-design.md
git commit -m "docs(spec): master-spec touch-ups for melody engine rework — MELODY/VARIATION, gate layer, capture removed"
```

---

## Task 11: Final verification — determinism, no-libDaisy, full suite, demo renders

**Files:**
- Test: `tests/test_variation.cpp` (add the determinism case) or a small `tests/test_determinism.cpp`

- [ ] **Step 1: Add a bit-determinism scenario test**

Add to `tests/test_variation.cpp` a case that renders a mixed scenario twice through the lane and asserts identical output — exercising GROW, RENEW, density and a principle switch:
```cpp
TEST_CASE("determinism: identical drive -> identical output across GROW/RENEW/density") {
    auto run = [](uint32_t seed) {
        ModLane l;
        l.set_melodic(true);
        l.set_principle(Principle::TwoMotif);
        l.init(48000.f, seed);
        l.set_shape(1.0f);
        l.set_step(true, 16);
        l.set_rate_hz(8.0f);
        std::vector<float> out;
        for (int n = 0; n < 60000; ++n) {
            if (n == 5000)  l.set_variation(0.7f);
            if (n == 15000) l.set_density(0.3f);
            if (n == 25000) l.set_variation(-0.8f);
            if (n == 35000) { l.set_principle(Principle::CallResponse); l.new_phrase(); }
            if (n == 45000) l.set_density(1.0f);
            out.push_back(l.process());
        }
        return out;
    };
    auto a = run(0xDECAF); auto b = run(0xDECAF);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]); // bit-identical
}
```

- [ ] **Step 2: Full clean build + test**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach
source env.sh && rm -rf build && cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```
Expected: all tests pass from a clean build.

- [ ] **Step 3: Confirm `engine/` has no libDaisy include**

```bash
grep -rn "daisy\|libdaisy\|DaisySeed" engine/ | grep -iv "daisysp" || echo "OK: no libDaisy in engine/"
```
Expected: `OK` (daisysp math in the voice/engine layer is allowed; libDaisy hardware is not — confirm no hardware headers under `engine/`).

- [ ] **Step 4: Re-render the pinned demos and sanity-check them**

```bash
for s in demo_step_melody demo_density_sweep dorian_melody pentatonic_melody melody_then_drift; do
  ./build/render.exe host/render/scenarios/$s.json /tmp/$s.wav /tmp/$s.csv && echo "rendered $s";
done
```
Expected: each renders without error. Listen (or inspect CSV) for looping, evolving, musical melodies — not arpeggios, repeated notes, or note salad. If a retired-capture scenario is referenced anywhere (Makefile/README/CI), update the reference.

- [ ] **Step 5: Grep for leftover dead references**

```bash
grep -rn "probability\|capture\|replay\|entropy\|erode\|ERODE\|PROBABILITY" engine host tests | grep -v "set_entropy\" ||" | grep -viE "co-author|comment" || echo "clean"
```
Expected: only the intentional `"set_entropy"` scenario-action alias in `scenario.cpp` remains; no other live references.

- [ ] **Step 6: Commit + finish**

```bash
git add -A
git commit -m "test: bit-determinism across GROW/RENEW/density/principle; final verification"
```

- [ ] **Step 7: Use the finishing-a-development-branch skill**

Invoke `superpowers:finishing-a-development-branch` to decide merge/PR/cleanup for `melody-engine-rework`.

---

## Self-Review

**Spec coverage:**
- §1 Melody data model → Tasks 1, 2, 5. ✓
- §2 MELODY knob / set_variation (LOOP/GROW/RENEW, per-unit dice, ev decay, live step change, GROW×RENEW behaviours) → Tasks 6, 7, 8. ✓
- §3 PhraseGen (Principle, PhraseLayout, generate_phrase, regenerate_unit, five principles, contour walk, metric weight) → Tasks 1–3. ✓
- §4 Gate layer + DENSITY (effective gate, STEP-only, FLOW no-freeze, `_frozen` change) → Task 6. ✓
- §5 New-phrase gesture (pending flag, cycle-wrap, coalesce, FLOW pending) → Task 8. ✓
- §6 Hardware mapping → concept only; wiring deferred to M6 (spec says so) — not in this plan. ✓
- Module changes table (lane.h/.cpp, phrase_gen.h, capture.h removed, super_modulator, instrument, host/render) → Tasks 1–9. ✓
- Naming (set_entropy→set_variation, MELODY label) → Task 7 + Task 10 (doc). ✓
- Testing section bullets → Tasks 1–3, 6, 7, 8, 11. ✓
- Master-spec touch-ups → Task 10. ✓
- Demos → Task 9 + Task 11 re-render. ✓
- Non-goals respected: no user-editable generator constants (fixed, by-ear); no scale logic in ModLane/PhraseGen (root = 0); no non-PITCH motif/gate/principles (`_fill_walk` only); no capture; no dice-based DENSITY (metric-ordered); one lane RNG. ✓

**Deferred to execution (by-ear tuning, per spec Non-goals):** exact walk widths, gravity, gate density thresholds, and the OneMotif A′/A″ micro-variation (delegated to GROW) are tuned during implementation; the plan ships correct, deterministic defaults that satisfy the structural tests.

**Type consistency:** `Principle`, `PhraseLayout` (fields `motif_len/tail_len/inst_count/motif_count`), `generate_phrase`/`regenerate_unit` signatures, `set_variation`/`set_density`/`set_principle`/`new_phrase`/`gate_state`/`pitch_gate`/`set_melodic` names are used identically across all tasks. `motif_count` = renewal-unit count throughout. RNG API is `next_unipolar`/`next_bipolar`/`next_u32` (no `next()`).

---

## Execution Handoff

Once this plan is saved, per the user's instruction: create branch `melody-engine-rework` (Task 0) and execute **subagent-driven** — a fresh subagent per task with a review checkpoint between tasks (REQUIRED SUB-SKILL: superpowers:subagent-driven-development).
