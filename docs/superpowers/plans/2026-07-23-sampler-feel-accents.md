# Sampler FEEL Accents Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DENS stops reaching the sampler's STEP path entirely, rolls are removed, and COLOR becomes FEEL in STEP — the depth with which each grain inherits the attack strength of its own transient (spec `docs/superpowers/specs/2026-07-23-sampler-feel-accents-design.md`).

**Architecture:** `Grain` gains a per-grain gain latched at spawn. The roll mechanism (`_retrig_period`, `_retrig_ctr`, the retrigger block in `process()`, `_walk_ref`) is deleted outright, shrinking the STEP draw contract from three draws per fire to two and forcing one deliberate re-baselining of the STEP golden vector. The STEP grain ceiling becomes a fixed constant instead of a DENS-derived one, `_next_ratio` pins STEP to the latched single tone, and Part pushes the raw COLOR knob to the engine as FEEL through the same sampler-only side channel as `set_overlap`/`set_step_clock`. FLOW is untouched throughout.

**Tech Stack:** C++17, no heap in `engine/` (host-injected memory only), doctest, CMake + Ninja + clang (desktop), deterministic Rng with documented draw order.

## Global Constraints

- **Build/test loop (always this, never MSVC):**
  `cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="<filter>"`
  Full suite: `./build/spky_tests` (exit code 0 = green). Suite is at 578 checks-green on `main` before this plan starts.
- **No heap, no libDaisy includes in `engine/`.** Fixed-size arrays only.
- **Determinism:** every random draw goes through the engine's `Rng`; draw counts must not depend on outcomes (draw always, apply conditionally). The STEP draw contract after this plan is exactly **two draws per fire: walk, then pan**, both drawn before any early return.
- **FLOW must stay bit-identical.** The FLOW golden vector test ("sampler: golden vector -- Rng draw order and SOURCE mapping are locked", `tests/test_sampler_engine.cpp:923`) must keep passing **unchanged**. No task in this plan may edit its table.
- **The STEP golden vector is re-baselined exactly once, in Task 2**, following the RE-BASELINING PROCEDURE written into the test itself (`tests/test_sampler_engine.cpp:3047-3063`): predict first, recapture wholesale, record the comparison dated. No other task may touch that table; if a later task moves it, STOP and report rather than recapturing again.
- **Accents draw nothing.** The per-grain gain is a pure function of the knob value and the stored marker strength.
- **No per-sample `std::pow`/`exp2f`:** trigger-rate and control-rate calls are fine (the `scan_rate()` precedent), the per-sample audio path is not.
- **Commit trailer (every commit):** `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — never the default Claude/Anthropic trailer.
- New tuning constants go to `engine/sampler/sampler_config.h` with a comment stating ear-tunable vs contract, matching the file's style.
- **Deleting a test is a reported decision, not a silent cleanup.** Every test this plan removes is named in the task that removes it. Removing anything not named there requires reporting it in the task report with the reason.

---

### Task 1: `Grain` carries a per-grain gain

**Files:**
- Modify: `engine/sampler/grain.h` (the header comment above `spawn`, the `spawn` signature, the pan-gain block, the member list)
- Modify: `engine/sampler/sampler_engine.cpp:705` and `:921` (the two engine call sites)
- Test: `tests/test_grain.cpp`

**Interfaces:**
- Produces (Task 5 relies on this exact signature):
  `void Grain::spawn(float start, float ratio, float pan, int len, int atk, int dec, bool reverse, float gain = 1.f)`

**Design decisions the implementer must not re-litigate:**

1. **The gain is folded into `_gl`/`_gr` at spawn, not multiplied per sample.** The spec's Architecture section describes "one multiplication per grain and sample"; folding it into the two pan gains is arithmetically identical on the output (`outL = l * w * _gl`) and costs *nothing* per sample. `release()` and `trim_total()` freeze `_level()`, which stays the pure window — the frozen value is scaled by `_gl`/`_gr` afterwards exactly as the running window is, so fades stay continuous and inherit the gain automatically. Report this as a deliberate deviation from the spec's wording in the task report.

2. **The header comment above `spawn` currently forbids exactly this** ("No overlap-normalization gain is latched here. That was tried … and measured worse"). That paragraph is about the *overlap-normalization* factor `1/sqrt(active)`, which must track the live grain count and is therefore wrong the moment it is latched. An accent gain is constant for the grain's whole life by construction. Amend the comment to say both things; do not delete it.

3. **A default argument, but the two engine call sites pass explicitly.** The default keeps ~25 existing `test_grain.cpp` call sites compiling untouched, which keeps this task's diff readable. The engine must still name the value at both call sites so the accent is visible where it is decided.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_grain.cpp`:

```cpp
// The accent gain (spec 2026-07-23 feel-accents) is latched at spawn and
// scales the whole grain -- window, release fade and all. Two grains
// identical but for the gain must differ by exactly that factor at EVERY
// sample, not just on the plateau: folding the gain into the pan gains is
// what makes that true, and a gain applied only to the plateau (or only
// outside a fade) would break this at the window edges.
TEST_CASE("grain: the spawn gain scales the whole window, sample for sample") {
    SampleBuffer buf;
    static SampleBuffer::Frame mem[4096];
    buf.set_memory(mem, 4096);
    for (size_t i = 0; i < 4096; ++i) { mem[i].l = 0.5f; mem[i].r = -0.5f; }
    buf.set_rec_size(4096);

    spky::Grain full, half;
    full.spawn(0.f, 1.f, 0.f, 400, 40, 40, false);          // default gain 1
    half.spawn(0.f, 1.f, 0.f, 400, 40, 40, false, 0.5f);

    for (int i = 0; i < 400; ++i) {
        float fl = 0.f, fr = 0.f, hl = 0.f, hr = 0.f;
        full.process(buf, fl, fr);
        half.process(buf, hl, hr);
        INFO("sample ", i);
        CHECK(hl == doctest::Approx(fl * 0.5f).epsilon(1e-6));
        CHECK(hr == doctest::Approx(fr * 0.5f).epsilon(1e-6));
    }
}

// The same factor must survive a release fade: release() freezes the WINDOW
// level, and the gain is applied after it, so the ratio holds through the
// fade too. A gain multiplied into the frozen level instead would square
// itself here.
TEST_CASE("grain: the spawn gain survives a release fade") {
    SampleBuffer buf;
    static SampleBuffer::Frame mem[4096];
    buf.set_memory(mem, 4096);
    for (size_t i = 0; i < 4096; ++i) { mem[i].l = 0.5f; mem[i].r = 0.5f; }
    buf.set_rec_size(4096);

    spky::Grain full, half;
    full.spawn(0.f, 1.f, 0.f, 4000, 40, 40, false);
    half.spawn(0.f, 1.f, 0.f, 4000, 40, 40, false, 0.5f);
    for (int i = 0; i < 500; ++i) {          // well onto the plateau
        float a = 0.f, b = 0.f;
        full.process(buf, a, b);
        half.process(buf, a, b);
    }
    full.release(200);
    half.release(200);
    for (int i = 0; i < 200; ++i) {
        float fl = 0.f, fr = 0.f, hl = 0.f, hr = 0.f;
        full.process(buf, fl, fr);
        half.process(buf, hl, hr);
        INFO("fade sample ", i);
        CHECK(hl == doctest::Approx(fl * 0.5f).epsilon(1e-6));
    }
}
```

Match the file's existing include and buffer-setup idiom if it differs from the sketch above — read the top of `tests/test_grain.cpp` and the first existing TEST_CASE and follow it. The assertions are the contract; the scaffolding is not.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="grain: the spawn gain*"
```

Expected: compile error — `spawn` takes 7 arguments, 8 given.

- [ ] **Step 3: Add the parameter**

In `engine/sampler/grain.h`, change the signature and the pan block:

```cpp
    void spawn(float start, float ratio, float pan, int len,
               int atk, int dec, bool reverse, float gain = 1.f) {
```

and at the end of `spawn`, replace the two pan-gain lines with:

```cpp
        // Equal-power pan, the Voice idiom (synth/voice.cpp:89-93):
        // angle 0..0.25 turns, gr = sin(a), gl = sin(a + quarter turn).
        // The accent gain rides ON the pan gains rather than on the window:
        // process() already multiplies by both, so this costs nothing per
        // sample, and _level() stays the pure window -- which is what makes
        // release()/trim_total() inherit the gain instead of squaring it.
        const float a = (clampf(pan, -1.f, 1.f) + 1.f) * 0.125f;
        const float g = gain < 0.f ? 0.f : gain;
        _gr = fast_sin(a) * g;
        _gl = fast_sin(a + 0.25f) * g;
```

Amend the header comment above `spawn` — keep the existing paragraph and add, at its end:

```
    // That argument is about the OVERLAP-NORMALIZATION factor specifically:
    // 1/sqrt(active) has to track the live grain count, so latching it is
    // wrong by construction. The `gain` parameter added 2026-07-23 (spec
    // feel-accents) is the opposite case -- an accent read from the grain's
    // own transient, constant for the grain's whole life. Latching it is not
    // just safe there, it is the only correct reading. The two must not be
    // confused: nothing may route _norm through this parameter.
```

- [ ] **Step 4: Name the gain at both engine call sites**

`engine/sampler/sampler_engine.cpp:705` (`_spawn_one`, the FLOW path):

```cpp
    // FLOW passes 1.f explicitly: COLOR means chord here, not accent, and the
    // spec (2026-07-23) keeps it that way. Named rather than defaulted so the
    // FLOW/STEP split is visible at the call site.
    _grains[slot].spawn(centre, ratio, pan, len, atk, dec, _reverse, 1.f);
```

`engine/sampler/sampler_engine.cpp:921` (`_spawn_slice`) — for now also `1.f`; Task 5 replaces it with the accent:

```cpp
    _grains[slot].spawn(pos, ratio, pan, len, atk, dec, _reverse, 1.f);
```

- [ ] **Step 5: Run the new tests, then the whole suite**

```bash
./build/spky_tests -tc="grain: the spawn gain*"
./build/spky_tests
```

Expected: both new cases pass; the full suite is green with no other case changed. In particular the FLOW golden vector must still pass — passing `1.f` explicitly cannot move it, and if it does, STOP and report.

- [ ] **Step 6: Prove the tests are load-bearing (mutation check)**

Temporarily change `_gr = fast_sin(a) * g;` back to `_gr = fast_sin(a);` (leaving `_gl` scaled), rebuild, and run `./build/spky_tests -tc="grain: the spawn gain*"`. Expected: the right-channel CHECK in the first case fails. Revert the mutation. Record the observed failure text in the task report.

- [ ] **Step 7: Commit**

```bash
git add engine/sampler/grain.h engine/sampler/sampler_engine.cpp tests/test_grain.cpp
git commit -m "feat(sampler): let a grain carry a gain latched at spawn

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Remove rolls, shrink the draw contract, re-baseline the STEP golden vector

**Files:**
- Modify: `engine/sampler/sampler_engine.h` (members `_retrig_period`, `_retrig_ctr`, `_walk_ref`, `_phrase_weight`, `_phrase_steps`; accessors `retrig_period()`, `walk_offset()`; the `set_phrase_pos` doc comment)
- Modify: `engine/sampler/sampler_engine.cpp` (`set_flow`, `set_gate`, `_fire_slice`, `_spawn_slice`, `process`, `punch`, `set_phrase_pos`)
- Modify: `tests/test_sampler_engine.cpp` (remove the roll cases, rework two, re-baseline the STEP golden vector)
- Modify: `tests/test_sampler_part.cpp` (rework the phrase-position test at `:817`)

**Interfaces:**
- Consumes: Task 1's `Grain::spawn(..., float gain = 1.f)` — unchanged by this task.
- Produces: STEP draw contract of exactly two draws per fire (walk, pan); `_spawn_slice(int k, float pan)` keeps its `bool` return (Task 3 and Task 5 both build on it); `set_phrase_pos(int slot, int steps, float weight)` keeps its signature.

**This is the largest task in the plan and it must land as one commit-set**, because the deletions and the golden-vector recapture are inseparable: the suite is red in between.

- [ ] **Step 1: Delete the roll mechanism from the engine**

`set_flow` (`sampler_engine.cpp:127-141`) — the whole `_retrig_period = 0;` line and its long comment go; the function reduces to:

```cpp
void SamplerEngine::set_flow(bool flow) {
    _flow = flow;
}
```

(The `if (flow == _flow) return;` guard existed only to make the roll cleanup edge-triggered — with nothing left to clean up, drop it too.)

`set_gate` (`sampler_engine.cpp:157-177`) — remove the pre-early-return disarm and rewrite the leading comment:

```cpp
// The gate no longer ARMS anything in STEP: the fire itself spawns (see
// _fire_slice, called from trigger/trigger_chord). All that is left for the
// falling edge is to release what is sounding, each grain over its own DEC --
// so a composed note ends where the composer wrote it, with a window tail
// rather than the old fixed kBurstReleaseS burst.
//
// FLOW is untouched by this: it never read _gate in the scheduler.
void SamplerEngine::set_gate(bool on) {
    _gate = on;
    if (_flow) return;
    if (!on) {
        for (int i = 0; i < kGrains; ++i) {
            if (!_grains[i].active()) continue;
            int fade = static_cast<int>(_dec_ref[i]);
            if (fade < static_cast<int>(kRecordFade))
                fade = static_cast<int>(kRecordFade);
            _grains[i].release(fade);
        }
    }
}
```

`_fire_slice` (`sampler_engine.cpp:798-867`) — the draw block loses the roll draw, the wrap loses `_walk_ref`, and the whole arming block at the end goes:

```cpp
    if (_phrase_slot < _last_slot) { _cursor = 0; _walk = 0.f; }
    _last_slot = _phrase_slot;

    const float motion = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    const float wdraw = _rng.next_bipolar();           // 1st: walk
    const float pan   = _rng.next_bipolar() * motion;  // 2nd: pan
```

...and after `_spawn_slice`:

```cpp
    _spawn_slice(k, pan);
    ++_cursor;
```

Replace the function's leading draw-contract comment with:

```cpp
// One fire = one slice grain (spec 2026-07-22), accented from its own
// transient (spec 2026-07-23).
// --- Rng draw order is contract: walk, then pan. Both ALWAYS drawn, before
// any early return, so a fire dropped at the grain ceiling consumes exactly
// the same stream as one that lands. ---
```

Keep the existing `_hold` paragraph below it verbatim — it is still true and still worth its length.

`_spawn_slice` — delete the `_walk_ref = _walk;` line and its comment block (`:930-939`). The function keeps returning `bool`.

`process()` (`sampler_engine.cpp:968-991`) — the entire `else if (!_hold && _gate && _retrig_period > 0) { ... }` branch goes. The scheduling block becomes:

```cpp
    // --- scheduling ---
    // FLOW only. In STEP nothing schedules itself: a fire spawns exactly one
    // grain and the gate falling releases it (spec 2026-07-23 removed the
    // roll retrigger that used to live here).
    if (_flow && !_hold) {
        _spawn_ctr -= 1.f;
        if (_spawn_ctr <= 0.f) {
            _spawn_one();                    // zieht _spawn_jitter neu
            // _next_interval() bodet bereits auf kSpawnMinSamples, und
            // _spawn_ctr ist an dieser Stelle > -1, also bleibt die Summe
            // sicher positiv -- die alte `if (_spawn_ctr < 1.f)`-Klemme war
            // genau die Stelle, an der der Jitter den CPU-Boden unterlief.
            _spawn_ctr += _next_interval();
        }
    }
```

`punch()` (`sampler_engine.cpp:1102-1113`) — drop `_walk_ref = 0.f;` and its comment.

`set_phrase_pos` (`sampler_engine.cpp:1096-1100`) — `_phrase_steps` was already assigned-but-never-read before this change, and `_phrase_weight` becomes so with the roll gone. Drop both fields; keep the three-parameter signature, which is the documented Part→Sampler contract and is what `part.cpp` and the tests call:

```cpp
// steps and weight are accepted and ignored: the phrase-position push is a
// Part->Sampler contract with one live consumer left, the wrap detection on
// `slot`. weight fed the roll dice (removed 2026-07-23) and steps was never
// read at all. The parameters stay in the signature because the contract is
// the interesting thing to keep stable -- a future accent or swing feature
// wants exactly these two numbers back.
void SamplerEngine::set_phrase_pos(int slot, int /*steps*/, float /*weight*/) {
    _phrase_slot = slot;
}
```

In `sampler_engine.h`, delete the members `_walk_ref`, `_retrig_period`, `_retrig_ctr`, `_phrase_steps`, `_phrase_weight`, and the accessors `walk_offset()` (`:233`) and `retrig_period()` (`:235`) with their comments. Update the `set_phrase_pos` declaration comment (`:189-192`) — its last clause "the roll dice reads it inverted" is now false. Update the class doc comment at `:50-54` if it mentions rolls.

- [ ] **Step 2: Delete the roll tests**

Remove these TEST_CASEs from `tests/test_sampler_engine.cpp` **in full, comments included** — they test a feature that no longer exists:

- `"sampler STEP: an off-beat at DENS max rolls at exactly step/subdiv"` (:2778)
- `"sampler STEP: DENS min never rolls, whatever the dice say"` (:2809)
- `"sampler STEP: DENS above the floor but below subdiv 2 still never rolls"` (:2821)
- `"sampler STEP: DENS just past subdiv 2 does roll -- the block is live"` (:2834)
- `"sampler STEP: a roll does not survive a FLOW round trip"` (:2865)
- `"sampler STEP: a fire that was dropped arms no roll"` (:2895)
- `"sampler STEP: the phrase wrap resets the roll's walk reference too"` (:2938)
- `"sampler STEP: downbeats mostly hit once, offs roll -- the bias is real"` (:3004)

Also remove the `// --- Task 7: rolls ---` section banner at :2776.

**Do not** remove `"sampler STEP: a dropped fire still consumes its Rng draws"` (:2665) — it is the draw-contract test and survives with an edit (Step 4).

- [ ] **Step 3: Add the replacement test — no retrigger, at any knob setting**

The spec lists "Kein Retrigger mehr" as a required test. Deleting the roll cases removes coverage of the *absence*; this puts it back. Add where the roll section used to be:

```cpp
// --- rolls are gone (spec 2026-07-23) --------------------------------------
//
// The deletion needs a positive test, not just the absence of the old ones:
// a held note must spawn exactly once, at every DENS and every metric weight
// the old arming block used to read. Restore the retrigger branch in
// process() and this fails at the first off-beat.
TEST_CASE("sampler STEP: a held note never retriggers, at any DENS or weight") {
    for (float dens : {0.f, 0.5f, 1.f})
        for (float weight : {0.f, 0.2f, 1.f}) {
            StepRig g;
            g.e.set_overlap(dens);
            g.render(96);
            const int before = g.e.spawn_count();
            g.fire(/*slot*/1, /*steps*/8, weight);
            g.render(6000 * 4);            // four whole steps under the gate
            INFO("DENS ", dens, " weight ", weight);
            CHECK(g.e.spawn_count() == before + 1);
            g.note_off();
            g.render(600);
            CHECK(g.e.spawn_count() == before + 1);
        }
}
```

- [ ] **Step 4: Fix the two tests that rode a roll**

**(a) `tests/test_sampler_engine.cpp:833`, `"sampler: a latched single-note STEP roll holds its ratio while PITCH drifts underneath it"`.** Read it in full first. It exists to prove `_next_ratio`'s latched branch does not track live pitch, and it reached a second spawn *via a roll retrigger*. With rolls gone, the second spawn must come from a second fire while the pitch drifts between them. Rename it to `"sampler: a latched STEP fire holds its ratio while PITCH drifts underneath it"`, drop the `REQUIRE(g.e.retrig_period() > 0)` line and the DENS/weight setup that existed only to force a roll, and restructure to:

1. fire once at pitch 0.5, render 64, record `last_spawn_ratio()`;
2. without a new `trigger`, drift `_chord[0]` by pushing a different PITCH target through `set_targets` and rendering ≥ 96 samples so `_update_control` refreshes `_chord_ratio[]`;
3. fire again **without** calling `trigger` in between is impossible in STEP (a fire only happens on trigger) — so instead assert on the *first* fire's grain: after the drift, `last_spawn_ratio()` must still read the value latched at the trigger, unchanged.

Keep the test's existing explanatory comment about *why* the two caches (`_burst_ratio` vs `_chord_ratio`) are separate, and add a dated line saying the roll vehicle was removed with the feature and what replaced it.

**(b) `tests/test_sampler_engine.cpp:2665`, `"sampler STEP: a dropped fire still consumes its Rng draws"`.** The mechanism is unchanged (draws happen before `_spawn_slice`'s ceiling check), but the golden literal at the end is now the **second** draw of the stream, not the third, so the value moves. Update the comment from "where pan is the THIRD draw of the stream" to "the SECOND draw" and recapture that one literal:

```bash
./build/spky_tests -tc="sampler STEP: a dropped fire still consumes its Rng draws"
```

Read the actual value out of the failure message and paste it in. This is a single derived literal, not a golden table — recapturing it is expected here, unlike the table in Step 6.

**(c) `tests/test_sampler_part.cpp:817`, `"part: the phrase position reaches the sampler -- off-beat fires arm rolls"`.** This test proved that `Part` actually pushes `set_phrase_pos` — and did it through roll arming, which is gone. The remaining observable consumer of the push is the **phrase wrap**: without the push, `_phrase_slot` stays 0 forever, no wrap is ever detected, and the slice cursor climbs monotonically instead of returning home. Replace it with:

```cpp
// Part must push set_phrase_pos before every fire. With the roll gone (spec
// 2026-07-23) the one remaining observable consumer is the phrase wrap:
// _fire_slice resets the cursor when the slot number goes BACKWARDS, so a
// sampler driven through more than one phrase cycle at MOTION 0 must replay
// the same slice order. Delete Part's set_phrase_pos call and _phrase_slot
// stays 0 forever, no wrap is ever seen, and the cursor climbs straight
// through the second cycle -- which this test catches as a slice sequence
// that never returns to its start.
TEST_CASE("part: the phrase position reaches the sampler -- the wrap sends the cursor home") {
```

Build it on the existing rig in that file (read the neighbouring sampler-part tests and reuse their setup verbatim rather than inventing one). The rig must have MOTION at 0 and inactive so the walk is structurally silent, and enough recorded transients that `slice_count() >= kMinSlices`. Drive it through **two full phrase cycles**, collecting `sampler_last_slice`-equivalent readings per fire, and assert the second cycle's sequence equals the first's. If the file has no accessor reaching `SamplerEngine::last_slice()` from a `Part`, use whatever observation seam the neighbouring tests already use for sampler state; do not add a new accessor for this.

- [ ] **Step 5: Build and confirm exactly the expected failures**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests
```

Expected at this point: everything green **except** the STEP golden vector case, which fails on `REQUIRE(got.size() == n)` — 12 rows produced against 44 expected. Any *other* failure means something in Steps 1–4 went beyond the intended change: STOP and report rather than proceeding to the recapture.

- [ ] **Step 6: Re-baseline the STEP golden vector — prediction first**

Follow the RE-BASELINING PROCEDURE in the test's own comment (`tests/test_sampler_engine.cpp:3047`). Write the prediction into the task report **before** capturing anything. The prediction this plan asserts:

1. **The table becomes 12 rows**, one per fire, since retriggers were the only source of extra spawns.
2. **Row 0 is byte-identical in `slice`, `pos` and `len`** (`{1, 5904.f, …, 2611}`): the walk is still the *first* draw of the stream, so the first fire's slice is untouched. Its `pan` changes, because pan is now drawn from what used to be the roll's draw.
3. **Rows 1..11 may all move**, in every column: each fire now consumes two draws instead of three, so the stream alignment shifts cumulatively from fire 1 onward.
4. **`len` stays drawn from the same set {2569, 2611, 2653}** — those come from SliceMap's marker spacing, which this plan does not touch. A `len` outside that set means the walk or the marker map moved and the recapture is not clean.

If the captured table contradicts any of the four, STOP and report — the change did more than intended.

- [ ] **Step 7: Recapture wholesale**

Print the produced rows rather than hand-fitting them. Add a temporary `MESSAGE` inside the collection loop (or dump `got` with a temporary loop before the comparison) emitting each row in the exact literal form `{ %d, %.1ff, %.6ff, %d },`, run:

```bash
./build/spky_tests -tc="sampler STEP: golden vector*" -s
```

and paste the printed block over the whole table. Remove the temporary printing afterwards. Never hand-edit individual rows toward green.

Then update the case's comment block:

- the draw-order line at :3023-3024 becomes `per fire: walk, then pan. There are no retriggers`;
- the paragraph at :3033-3037 loses "downbeats do not roll, the roll period is step/subdiv" from its list of margins;
- the "44 grains here, and that COUNT is pinned too" paragraph at :3114-3118 is rewritten for 12 — one row per fire, and the count is now pinned as *exactly the fire count*, which is itself the assertion that nothing retriggers;
- the closing note at :3061-3063 gains a dated **RECAPTURED** entry: the date (2026-07-23), the reason (rolls removed, spec `2026-07-23-sampler-feel-accents-design.md`), the prediction from Step 6, and whether the capture matched it point for point.

The `g.e.set_overlap(1.f)` line at :3074 with its "DENS max: subdiv cap 8, rolls live" comment stays — DENS still sets the grain ceiling until Task 3 — but the comment becomes "DENS max: the grain ceiling is still DENS-derived here (Task 3 fixes it)".

- [ ] **Step 8: Full suite**

```bash
./build/spky_tests
```

Expected: green, exit 0. The FLOW golden vector must be untouched and passing.

- [ ] **Step 9: Prove the new no-retrigger test is load-bearing**

Re-add a minimal retrigger to `process()` temporarily (spawn a second grain in STEP after 1000 samples under the gate), rebuild, run `./build/spky_tests -tc="sampler STEP: a held note never retriggers*"`. Expected: fails. Revert. Record the failure text in the report.

- [ ] **Step 10: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp \
        tests/test_sampler_engine.cpp tests/test_sampler_part.cpp
git commit -m "feat(sampler)!: remove rolls, shrink the STEP draw contract to walk+pan

Re-baselines the STEP golden vector (44 -> 12 rows) with the prediction and
the comparison recorded in the test, per its own procedure.

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: A fixed grain ceiling in STEP

**Files:**
- Modify: `engine/sampler/sampler_config.h` (append `kStepGrainCeil`)
- Modify: `engine/sampler/sampler_engine.cpp:891` (`_spawn_slice`'s ceiling)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: `_spawn_slice(int k, float pan) -> bool` from Task 2.
- Produces: `sampler_cfg::kStepGrainCeil` (an `int`), read by nothing outside `_spawn_slice`.

**Why the value is 10 and not something rounder:** it equals the old ceiling at DENS maximum (`kOverlapMax + kSpawnHeadroom` = 8 + 2), so the measured worst-case CPU load does not rise. It is a notional emergency stop, not a musical control — at DENS minimum the old ceiling was 3, which silently swallowed composed notes whenever LEN was long.

- [ ] **Step 1: Write the failing test**

```cpp
// The STEP grain ceiling is fixed (spec 2026-07-23): DENS must not be able
// to swallow composed notes. At DENS minimum and long LEN the old ceiling
// was ceil(1) + kSpawnHeadroom = 3, so the fourth held fire onwards was
// dropped in silence. Every fire must land now.
TEST_CASE("sampler STEP: DENS minimum no longer drops fires at long LEN") {
    StepRig g;
    g.e.set_overlap(0.f);                  // DENS min
    g.feed(/*pitch*/0.5f, /*source*/0.f, /*size*/1.f, /*motion*/0.f);  // SIZE 1: grains outlive the fires
    g.render(96);
    const int dropped_before = g.e.dropped_spawns();
    const int spawned_before = g.e.spawn_count();
    const int kFires = 8;                  // more than the old ceiling of 3
    for (int i = 0; i < kFires; ++i) {     // hold every note: they pile up
        g.fire(i);
        g.render(64);
    }
    CHECK(g.e.dropped_spawns() == dropped_before);
    CHECK(g.e.spawn_count() == spawned_before + kFires);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler STEP: DENS minimum no longer drops fires*"
```

Expected: FAIL — `dropped_spawns()` has grown, and `spawn_count()` is short of 8.

- [ ] **Step 3: Add the constant**

Append to `engine/sampler/sampler_config.h`, inside `namespace sampler_cfg`, next to the other slice-groove constants:

```cpp
// --- feel accents (spec 2026-07-23 sampler-feel-accents-design.md) ---
// STEP polyphony ceiling, fixed. DENS used to set it (ceil(_overlap) +
// kSpawnHeadroom), which meant turning DENS down to thin the phrase ALSO
// dropped composed notes -- silently, with nothing on the panel to show it.
// 10 == the old ceiling at DENS maximum (kOverlapMax + kSpawnHeadroom), so
// the worst case this was measured at is unchanged; it is an emergency stop,
// not a control. NOT ear-tunable: it is a CPU budget.
constexpr int    kStepGrainCeil = 10;
```

- [ ] **Step 4: Use it**

`engine/sampler/sampler_engine.cpp`, in `_spawn_slice`, replace:

```cpp
    const int ceiling = static_cast<int>(std::ceil(_overlap)) + kSpawnHeadroom;
```

with:

```cpp
    // Fixed, not DENS-derived (spec 2026-07-23): DENS must not be able to
    // swallow composed notes. FLOW's _spawn_one keeps its DENS-derived
    // ceiling -- there the density knob genuinely IS the cloud density.
    const int ceiling = kStepGrainCeil;
```

Check whether `<cmath>`'s `std::ceil` is still used elsewhere in the file before touching includes — `_spawn_one` also calls it, so it almost certainly stays.

- [ ] **Step 5: Run the test, then the suite**

```bash
./build/spky_tests -tc="sampler STEP: DENS minimum no longer drops fires*"
./build/spky_tests
```

Expected: both green. The STEP golden vector runs at DENS max, where the old ceiling was also 10 — **it must not move**. If it does, STOP and report; do not recapture it (Global Constraints).

- [ ] **Step 6: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): fix the STEP grain ceiling instead of deriving it from DENS

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: STEP pins the chord to one tone

**Files:**
- Modify: `engine/sampler/sampler_engine.cpp:337-366` (`_next_ratio`)
- Modify: `engine/sampler/sampler_engine.h` (remove `_burst_latched`, update the `_burst_ratio` comment)
- Modify: `engine/sampler/sampler_engine.cpp:218` and `:232` (the two `_burst_latched = true;` assignments)
- Test: `tests/test_sampler_engine.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `_next_ratio()` returns `_burst_ratio` unconditionally in STEP. Task 5 does not touch it.

**Why:** in STEP a fire spawns exactly one grain, so a chord round-robin arpeggiates one note per beat rather than making a cloud. COLOR must mean FEEL there and nothing else, or this plan simply moves the DENS overload onto a different knob. A side effect worth naming in the commit: the known `_next_ratio` spec deviation documented at `:346-359` (chords are *not* frozen at the gate, contrary to the texture-deck spec) is settled by making that branch unreachable in STEP.

- [ ] **Step 1: Write the failing test**

```cpp
// COLOR means FEEL in STEP, not chord size (spec 2026-07-23). The chord
// round-robin must be unreachable there: every fire plays the tone latched
// at its own trigger, whatever COLOR says. Restore the `_chord_n <= 1`
// guard and the four fires below start walking a chord instead.
TEST_CASE("sampler STEP: the chord is pinned to the triggered tone at every COLOR") {
    const float notes[4] = { 0.30f, 0.45f, 0.60f, 0.75f };
    StepRig g;
    for (int i = 0; i < 4; ++i) {
        g.e.set_phrase_pos(i, 8, 1.f);
        g.e.trigger_chord(notes, 4);       // a four-note chord: COLOR wide open
        g.e.set_gate(true);
        g.render(64);
        // Every fire reads the chord's ROOT (notes[0], what trigger_chord
        // latches as _burst_pitch), never notes[1..3] in rotation.
        INFO("fire ", i);
        CHECK(g.e.last_spawn_ratio() == doctest::Approx(g.e.last_spawn_ratio()));
        CHECK(g.e.last_spawn_ratio() == doctest::Approx(spky::test_ratio_for(notes[0])).epsilon(1e-5));
        g.e.set_gate(false);
        g.render(6000);
    }
}

// FLOW is the other half of the same contract: there the round-robin is the
// point, and COLOR still builds a chord cloud. Four spawns must NOT all read
// the same ratio.
TEST_CASE("sampler FLOW: a chord still rotates through its notes") {
    const float notes[4] = { 0.30f, 0.45f, 0.60f, 0.75f };
    StepRig g;
    g.e.set_flow(true);
    g.e.set_chord(notes, 4);
    g.render(96);
    std::vector<float> ratios;
    int last = g.e.spawn_count();
    for (int i = 0; i < 48000 && ratios.size() < 4; ++i) {
        float a = 0.f, b = 0.f;
        g.e.process(a, b);
        if (g.e.spawn_count() != last) { last = g.e.spawn_count(); ratios.push_back(g.e.last_spawn_ratio()); }
    }
    REQUIRE(ratios.size() == 4);
    bool all_same = true;
    for (size_t i = 1; i < ratios.size(); ++i)
        if (std::fabs(ratios[i] - ratios[0]) > 1e-5f) all_same = false;
    CHECK_FALSE(all_same);
}
```

Drop the tautological first CHECK in the STEP case if it survived the sketch — it asserts nothing. The load-bearing assertion is the comparison against `test_ratio_for(notes[0])`.

- [ ] **Step 2: Run it to verify it fails**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler STEP: the chord is pinned*,sampler FLOW: a chord still rotates*"
```

Expected: the STEP case FAILS (fires 1..3 read rotated chord tones); the FLOW case PASSES already — it pins existing behaviour so this task cannot break it.

- [ ] **Step 3: Implement**

Replace `_next_ratio`'s body (`sampler_engine.cpp:337-366`) with:

```cpp
float SamplerEngine::_next_ratio() {
    // STEP pins the chord to one tone (spec 2026-07-23 feel-accents). A fire
    // spawns exactly ONE grain, so a round-robin there is an arpeggio, not a
    // cloud -- and COLOR has to mean FEEL in STEP and nothing else, or the
    // DENS overload this spec removes just moves to another knob.
    //
    // _burst_ratio is frozen at the trigger that set it, not _chord_ratio[0]
    // -- see the comment at _burst_ratio's declaration for why these are two
    // caches and not one. Its 1.0 default (== ratio_for(0.5f)) makes a spawn
    // that somehow precedes any trigger read unity, not silence or NaN.
    //
    // Side effect worth recording: the deviation this function used to carry
    // -- chords were NOT frozen at the gate, contrary to the texture-deck
    // spec -- is settled rather than fixed. The chord path is simply not
    // reachable in STEP any more.
    if (!_flow) return _burst_ratio;

    // FLOW: the round-robin IS the chord cloud, and COLOR still sizes it.
    // Reads _chord_ratio[] live, refreshed every control tick.
    const int idx = _rr % _chord_n;   // capture before _rr advances
    _rr = (_rr + 1) % _chord_n;
    return _chord_ratio[idx];
}
```

Keep the file-level comment above the function (the one about MOTION's octave scatter and its removed draws) unchanged.

Then delete `_burst_latched`: the member declaration in `sampler_engine.h:338` with its comment, and the two `_burst_latched = true;` assignments at `sampler_engine.cpp:218` and `:232`. Verify with `grep -rn "_burst_latched" engine/ tests/` that nothing remains.

- [ ] **Step 4: Run the tests, then the suite**

```bash
./build/spky_tests -tc="sampler STEP: the chord is pinned*,sampler FLOW: a chord still rotates*"
./build/spky_tests
```

Expected: both green, full suite green. The STEP golden vector uses `trigger()` (a single tone, `_chord_n == 1`), which already took the latched path — **it must not move**. If it does, STOP and report.

- [ ] **Step 5: Commit**

```bash
git add engine/sampler/sampler_engine.h engine/sampler/sampler_engine.cpp tests/test_sampler_engine.cpp
git commit -m "feat(sampler): pin the chord to the triggered tone in STEP

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: FEEL — the accent gain

**Files:**
- Modify: `engine/sampler/sampler_config.h` (append `kAccentFloor`)
- Modify: `engine/sampler/sampler_engine.h` (declare `set_feel`, add `_feel`)
- Modify: `engine/sampler/sampler_engine.cpp` (define `set_feel`, compute the gain in `_spawn_slice`)
- Modify: `engine/parts/part.cpp` (push `_color` at the control tick)
- Test: `tests/test_sampler_engine.cpp`, `tests/test_sampler_part.cpp`

**Interfaces:**
- Consumes: `Grain::spawn(..., float gain)` (Task 1); `SliceMap::strength(int i) -> uint8_t` (`engine/sampler/slice_map.h:41`); `SamplerEngine::_marker_mode() const` (`sampler_engine.cpp:729`).
- Produces: `void SamplerEngine::set_feel(float n)`; `sampler_cfg::kAccentFloor` (a `float`).

**The formula, verbatim from the spec:**

```
a = FEEL, the raw COLOR knob value (NOT _color_eff -- no MOTION swing)
s = strength(k) / 255
gain = lerp(1, lerp(kAccentFloor, 1, s), a)
```

At `a = 0` every grain has gain exactly 1 — the flat reference the loop must be audible without. At `a = 1` the level follows the material between `kAccentFloor` and 1. In grid fallback (no markers, so no `strength`) the gain stays 1 at every knob position; that is the honest answer, not a defect.

- [ ] **Step 1: Write the failing tests**

```cpp
// FEEL (spec 2026-07-23): COLOR sets how deeply each grain inherits the
// attack strength of its own transient. The observable is the grain's peak
// output level, so drive a marker whose strength is known to be low and one
// known to be high, and compare.
//
// Detector note, load-bearing for reading this test: strength is the
// fast/slow envelope RATIO at the onset, not the peak amplitude -- the first
// click after silence scores 255 whatever its level. So the comparison below
// is between MARKERS, not between click amplitudes.
TEST_CASE("sampler STEP: FEEL 0 is exactly flat") {
    auto peak_of_fire = [](float feel, int slot) {
        StepRig g;
        g.e.set_feel(feel);
        g.render(96);
        g.fire(slot);
        float p = 0.f;
        for (int i = 0; i < 3000; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
            if (m > p) p = m;
        }
        return p;
    };
    // FEEL 0 is the reference the loop must be audible without: EVERY grain
    // plays at unity, whatever its marker says. The outer lerp returns
    // exactly 1 there -- structural, not approximate -- so this compares two
    // slices whose stored strengths differ against the same unaccented run.
    StepRig probe;
    REQUIRE(probe.e.slice_count() >= 3);
    for (int slot : {0, 1, 2}) {
        const float a = peak_of_fire(0.f, slot);
        const float b = peak_of_fire(0.f, slot);
        INFO("slot ", slot);
        REQUIRE(a > 0.f);
        CHECK(a == doctest::Approx(b));          // deterministic to begin with
        // and no accent at all: the FEEL-1 run of the same fire is quieter
        // or equal, never louder, since lerp(kAccentFloor, 1, s) <= 1.
        CHECK(peak_of_fire(1.f, slot) <= doctest::Approx(a).epsilon(1e-4));
    }
}

// The exact law, not just its direction: at FEEL 1 the ratio between a
// grain's peak and its FEEL-0 peak must equal lerp(kAccentFloor, 1, s) for
// that grain's own marker. This is the test that catches a floor applied at
// the wrong end, or s read off the wrong marker.
TEST_CASE("sampler STEP: the accent factor equals lerp(kAccentFloor, 1, s)") {
    auto fire_peak = [](float feel, int slot, int& slice_out) {
        StepRig g;
        g.e.set_feel(feel);
        g.render(96);
        g.fire(slot);
        float p = 0.f;
        for (int i = 0; i < 3000; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
            if (m > p) p = m;
        }
        slice_out = g.e.last_slice();
        return std::make_pair(p, g.e.slice_strength(g.e.last_slice()));
    };
    for (int slot : {0, 1, 2}) {
        int s0 = -1, s1 = -1;
        const auto flat = fire_peak(0.f, slot, s0);
        const auto felt = fire_peak(1.f, slot, s1);
        REQUIRE(s0 == s1);                       // same fire, same marker
        REQUIRE(flat.first > 0.f);
        const float s = static_cast<float>(flat.second) * (1.f / 255.f);
        const float want = sampler_cfg::kAccentFloor
                         + (1.f - sampler_cfg::kAccentFloor) * s;
        INFO("slot ", slot, " marker ", s0, " strength ", int(flat.second));
        CHECK(felt.first / flat.first == doctest::Approx(want).epsilon(1e-3));
    }
}

// Grid fallback: transientless material has no markers and therefore no
// strength. FEEL must be a no-op there, at every knob position -- not a
// silent duck to kAccentFloor.
TEST_CASE("sampler STEP: FEEL does nothing on transientless material") {
    auto peak_at = [](float feel) {
        Rig g;                              // default rig: 441 Hz sine, no clicks
        g.e.set_flow(false);
        g.e.set_step_clock(6000.f);
        REQUIRE(g.e.slice_count() < sampler_cfg::kMinSlices);
        g.e.set_feel(feel);
        g.e.set_phrase_pos(0, 8, 1.f);
        g.e.trigger(0.5f);
        g.e.set_gate(true);
        float p = 0.f;
        for (int i = 0; i < 3000; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            const float m = std::fabs(a) > std::fabs(b) ? std::fabs(a) : std::fabs(b);
            if (m > p) p = m;
        }
        return p;
    };
    const float off = peak_at(0.f);
    REQUIRE(off > 0.f);
    CHECK(peak_at(1.f) == doctest::Approx(off).epsilon(1e-5));
}

// FLOW never accents: COLOR means chord there and the grains stay at gain 1.
TEST_CASE("sampler FLOW: FEEL does not touch the cloud") {
    auto rms_at = [](float feel) {
        StepRig g;                          // 8 clicks -> plenty of markers
        g.e.set_flow(true);
        g.e.set_feel(feel);
        g.render(96);
        double acc = 0.0;
        const int n = 48000;
        for (int i = 0; i < n; ++i) {
            float a = 0.f, b = 0.f;
            g.e.process(a, b);
            acc += double(a) * a + double(b) * b;
        }
        return std::sqrt(acc / (2.0 * n));
    };
    const double off = rms_at(0.f);
    REQUIRE(off > 1e-4);
    CHECK(rms_at(1.f) == doctest::Approx(off).epsilon(1e-5));
}
```

`slice_strength(int)` is a new read-only observation accessor. Add it to
`engine/sampler/sampler_engine.h` beside the existing `slice_start(int)` at
`:205`, which is the precedent:

```cpp
    uint8_t slice_strength(int i) const { return _slices.strength(i); }
```

If any of the four cases needs a different rig or seam than the sketch assumes,
follow what the neighbouring tests in the file already do — the assertions are
the contract, the scaffolding is not.

- [ ] **Step 2: Run them to verify they fail**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target spky_tests && ./build/spky_tests -tc="sampler*FEEL*,sampler*accent factor*"
```

Expected: compile error — `set_feel` does not exist.

- [ ] **Step 3: Add the constant**

Append to `engine/sampler/sampler_config.h`, beside `kStepGrainCeil`:

```cpp
// FEEL: how quiet the WEAKEST transient may get at COLOR 1. gain =
// lerp(1, lerp(kAccentFloor, 1, s), feel), with s = strength/255.
// Ear-tunable, and explicitly so: too low and soft hits vanish from the
// loop, too high and the knob does nothing audible. 0.35 is a starting
// point, not a measurement -- it belongs in a listening pass.
//
// Read the strength note in the spec before tuning it: strength is the
// fast/slow envelope ratio at the onset, so it measures how SUDDEN a hit is,
// not how loud. The first hit after any gap scores 255 regardless of level.
constexpr float  kAccentFloor   = 0.35f;
```

- [ ] **Step 4: Add the side channel**

In `engine/sampler/sampler_engine.h`, beside `set_overlap`/`set_step_clock`:

```cpp
    // FEEL (spec 2026-07-23): COLOR on a sampler deck in STEP. Accent depth,
    // 0..1. At 0 every grain plays at unity -- the flat reference. Pushed
    // from Part at the control tick, from the RAW knob: COLOR's MOTION swing
    // (kColorMod) is right for the chord and wrong for accents, which must
    // not breathe. Ignored in FLOW, where COLOR still means chord.
    void set_feel(float n);
```

and the member, beside the other slice-groove state:

```cpp
    float _feel = 0.f;                // FEEL: accent depth, 0..1
```

and the observation accessor the Part-level test reads, beside `overlap()` at `:207`:

```cpp
    float feel() const { return _feel; }
```

In `engine/sampler/sampler_engine.cpp`, beside `set_overlap`:

```cpp
void SamplerEngine::set_feel(float n) { _feel = clampf(n, 0.f, 1.f); }
```

- [ ] **Step 5: Compute the gain**

In `_spawn_slice`, immediately before the `_grains[slot].spawn(...)` call:

```cpp
    // FEEL (spec 2026-07-23): the grain inherits the attack strength of its
    // own transient, as deeply as COLOR asks. At _feel == 0 the outer lerp
    // returns exactly 1 for every grain -- the flat reference, and the
    // kColorGate idiom's structural silence without a branch. In grid
    // fallback there is no marker and therefore no strength: gain stays 1 at
    // every knob position, which is the honest answer for transientless
    // material rather than a silent duck to the floor.
    float gain = 1.f;
    if (_marker_mode()) {
        const float s = static_cast<float>(_slices.strength(k)) * (1.f / 255.f);
        gain = lerpf(1.f, lerpf(kAccentFloor, 1.f, s), _feel);
    }

    _grains[slot].spawn(pos, ratio, pan, len, atk, dec, _reverse, gain);
```

(Replace the `1.f` Task 1 left at that call site.) Confirm `kAccentFloor` resolves — the file has a `using namespace sampler_cfg` or equivalent already, since `kSpawnHeadroom` and `kMinSlices` are used unqualified.

- [ ] **Step 6: Push it from Part**

In `engine/parts/part.cpp`, immediately after the `set_step_clock` push at `:234-235`:

```cpp
    // FEEL (spec 2026-07-23): COLOR reaches the sampler RAW, not as
    // _color_eff. The MOTION swing on COLOR (kColorMod, just above) is right
    // for the chord -- a breathing chord size is the point there -- and wrong
    // for accents: an accent depth that breathes would be exactly the hidden
    // coupling this spec exists to remove. Same sampler-only push idiom as
    // set_step_clock.
    if (_engine_id == ENGINE_SAMPLER)
        _sampler.set_feel(_color);
```

- [ ] **Step 7: Add the Part-level test**

In `tests/test_sampler_part.cpp`, beside the existing `"sampler part: the MOTION lane breathes the grain overlap"` (`:342`), which is the template for the rig and the MOTION setup:

```cpp
// FEEL reaches the sampler from the RAW COLOR knob, with no MOTION swing on
// it (spec 2026-07-23). The proof is a part whose MOTION lane is wide open:
// _color_eff visibly breathes (the chord test above pins that), but what the
// sampler receives must not. Push _color_eff instead and this fails.
TEST_CASE("sampler part: FEEL reaches the sampler unswung by MOTION") {
    std::vector<SampleBuffer::Frame> sbuf(kSFrames, SampleBuffer::Frame{ 0.f, 0.f });
    Part p;
    p.init(48000.f, 0, nullptr, nullptr, sbuf.data(), sbuf.size());
    p.set_engine(ENGINE_SAMPLER);
    p.set_color(0.5f);                  // mid-knob: the swing has room both ways
    p.set_depth(1.f);
    p.set_target_active(LANE_MOTION, true);

    float clo = 2.f, chi = -2.f, flo = 2.f, fhi = -2.f;
    for (int i = 0; i < 48000; ++i) {
        float a = 0.f, b = 0.f;
        p.process(a, b);
        const float c = p.color_eff();
        if (c < clo) clo = c;
        if (c > chi) chi = c;
        const float f = p.sampler().feel();
        if (f < flo) flo = f;
        if (f > fhi) fhi = f;
    }
    CHECK(chi > clo + 0.02f);            // _color_eff really breathes...
    CHECK(flo == doctest::Approx(0.5f)); // ...and FEEL really does not
    CHECK(fhi == doctest::Approx(0.5f));
}

- [ ] **Step 8: Run everything**

```bash
./build/spky_tests -tc="sampler*FEEL*,sampler*accent factor*"
./build/spky_tests
```

Expected: all green. The STEP golden vector pins `slice`, `pos`, `pan` and `len` — none of them is the gain, and FEEL draws nothing — so **it must not move**. If it does, STOP and report.

- [ ] **Step 9: Prove the accent law is load-bearing (mutation check)**

Change `lerpf(kAccentFloor, 1.f, s)` to `lerpf(1.f, kAccentFloor, s)` (floor at the wrong end), rebuild, and run the FEEL cases. Expected: `"the accent factor equals lerp(kAccentFloor, 1, s)"` fails. Revert. Then change `if (_marker_mode())` to an unconditional assignment, rebuild, and expect `"FEEL does nothing on transientless material"` to fail. Revert. Record both failure texts in the report.

- [ ] **Step 10: Commit**

```bash
git add engine/sampler/sampler_config.h engine/sampler/sampler_engine.h \
        engine/sampler/sampler_engine.cpp engine/parts/part.cpp \
        tests/test_sampler_engine.cpp tests/test_sampler_part.cpp
git commit -m "feat(sampler): COLOR becomes FEEL in STEP -- accents from the material

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Panel label, listening scenario, renders

**Files:**
- Modify: `host/vcv/res/gen_panel.py:397` (`SAMPLER_LBL`)
- Regenerate: `host/vcv/res/*.svg` (whatever `gen_panel.py` writes — read its `__main__` block for the output paths)
- Modify: `host/render/scenarios/sampler_slice_drums.json`
- Regenerate: `renders/sampler_slice_drums.wav`, `renders/sampler_slice_drums.csv`

**Interfaces:**
- Consumes: `set_color` as a render-scenario action (`host/render/scenario.cpp:113`), which reaches `Instrument::set_color` → `Part::set_color` → `_color` → Task 5's `set_feel` push.

- [ ] **Step 1: Add the second caption**

```python
SAMPLER_LBL = [("MELODY", "SCAN"), ("SUB", "LEN"), ("DETUNE", "ORG"), ("COLOR", "FEEL")]
```

Read the block at `gen_panel.py:400-410` and the `SAMPLER_RADIAL` handling at `:456-470` first: the inline captions sit on the caption's own baseline one gap behind it, and radial captions take a different branch. If COLOR is in `SAMPLER_RADIAL`, follow that branch's geometry rather than the inline one.

Note in the commit body the known imprecision the spec records: the second caption is engine-scoped but the meaning is mode-scoped, so a sampler deck reads FEEL at COLOR even though COLOR still means chord in FLOW. Deliberately accepted.

- [ ] **Step 2: Regenerate and eyeball the panel**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach/host/vcv && python res/gen_panel.py
git diff --stat res/
```

Expected: the SVG(s) change and nothing else. Check the new caption does not collide with COLOR's own glyphs or the neighbouring control — the file's text-width helper (`text_w`) exists for exactly this, and the other three captions are the reference for spacing.

- [ ] **Step 3: Swap the scenario's DENS ramp for a COLOR ramp**

In `host/render/scenarios/sampler_slice_drums.json`, replace the event at `t: 26.0`:

```json
    { "t": 26.0, "action": "set_color", "part": 1, "value": 1.0 }
```

and update the `_comment` field: the sentence "DENS max from 26s lets the offs roll." becomes a statement of what the render now demonstrates — COLOR to maximum at 26 s opens FEEL to full depth, so the second half of the render should show the loop's dynamics tracking the recorded material's transients. Keep the rest of the comment (the marker-mode note, the MOTION/`set_depth` note, the action-table note) intact; also drop the now-stale trailing sentence about `set_sampler_overlap` **only if** `sampler_overlap` no longer appears in the file.

- [ ] **Step 4: Re-render**

```bash
cd /c/Users/bernd/Documents/AI/Spotykach && source env.sh && cmake --build build --target render && \
  ./build/render.exe host/render/scenarios/sampler_slice_drums.json \
      renders/sampler_slice_drums.wav renders/sampler_slice_drums.csv
```

Expected: it writes the WAV and CSV without error. Check the CSV's slice-count column still sits well above `kMinSlices` = 4 (marker mode is what makes this scenario the counterpart to the field-recording grid fallback) and report the value.

Also re-render `sampler_slice_field.json` to the same paths it used before — its content should be unchanged by this plan except for the removed rolls, and a stale WAV beside a fresh one is worse than none.

- [ ] **Step 5: Full suite one more time**

```bash
./build/spky_tests
```

Expected: green. `tests/test_scenario.cpp` may parse the scenario files; if it does and it fails, the JSON edit is the cause.

- [ ] **Step 6: Commit**

```bash
git add host/vcv/res/gen_panel.py host/vcv/res/*.svg \
        host/render/scenarios/sampler_slice_drums.json \
        renders/sampler_slice_drums.wav renders/sampler_slice_drums.csv \
        renders/sampler_slice_field.wav renders/sampler_slice_field.csv
git commit -m "feat(panel): label COLOR as FEEL on a sampler deck, and re-render the drums scenario

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## After the plan

Two things belong to Bastian and must be reported, not decided:

1. **The listening pass.** `kAccentFloor = 0.35` is a starting point. The spec names, in ascending order of intervention, what to try if FEEL turns out inaudible or binary: (a) normalise `s` against the map's maximum strength — one line in the FEEL path; (b) have the detector store peak amplitude in the slice window instead of the envelope ratio. Neither is in scope here.
2. **The VCV plugin.** Build and install it so the change can actually be heard: `cd host/vcv && ./build-local.sh install` — never hand-rolled, the system `g++` is the ARM cross-compiler.

## Plan Self-Review

**Spec coverage** — every section of `2026-07-23-sampler-feel-accents-design.md` maps to a task:

| Spec section | Task |
|---|---|
| DENS: FLOW unchanged, STEP stops reading it | 2 (roll arming, `_fire_slice`'s DENS reads) + 3 (the ceiling) |
| COLOR = FEEL formula, raw knob, no MOTION swing | 5 |
| Grid fallback: gain stays 1 | 5 |
| Chord pinned in STEP | 4 |
| Rolls removed | 2 |
| Grain ceiling `kStepGrainCeil = 10` | 3 |
| Architecture 1: `Grain` gain | 1 |
| Architecture 2: FEEL side channel | 5 |
| Architecture 3: draw contract 2/fire, STEP golden re-pinned | 2 |
| "Was entfernt wird" (all six bullets) | 2 |
| Panel `SAMPLER_LBL` | 6 |
| Tests: accent depth, grid fallback, DENS decoupled, ceiling, chord pinned, no retrigger, FLOW untouched, STEP golden, drums scenario | 5, 5, 2+3, 3, 4, 2, 1+4+5 (every task re-runs the FLOW vector), 2, 6 |

**Gap found and closed while reviewing:** the spec's test list includes "DENS ist entkoppelt: eine DENS-Fahrt über den ganzen Weg ändert in STEP weder Spawn-Zeitpunkte noch Pegel". Task 2's no-retrigger test sweeps DENS across `{0, 0.5, 1}` and pins the spawn count, and Task 3's ceiling test pins the drop count at DENS minimum — together they cover the claim. Named here so a reviewer does not read it as missing.

**Known deviations from the spec, both deliberate and both to be reported by their implementer:**

- Task 1 folds the gain into the pan gains rather than adding a per-sample multiply. Identical output, cheaper. The spec's CPU section says "eine Multiplikation pro Grain und Sample"; this is zero.
- Task 2 additionally removes `_phrase_steps` and `_phrase_weight` (dead after the roll goes; `_phrase_steps` was dead before it) and `_burst_latched` moves to Task 4. The spec's removal list does not name them because they are consequences, not decisions.

**Type consistency:** `Grain::spawn`'s eighth parameter is `float gain` in Tasks 1 and 5; `set_feel(float)` and `_feel` in Task 5 only; `kStepGrainCeil` (`int`) in Task 3 only; `kAccentFloor` (`float`) in Task 5 only; `_spawn_slice(int, float) -> bool` unchanged throughout.
