# Groove Variation Zones — MELODY Knob Reaches the Rhythm

**Date:** 2026-07-16
**Status:** Approved design (user), pre-implementation
**Extends:** `2026-07-16-rhythm-groove-design.md` (ranked-slot groove). That spec's
"RENEW leaves the groove alone" lifecycle rule is superseded by the zone model below.
**Scope:** `engine/mod/phrase_gen.h`, `engine/mod/lane.cpp/.h`, tests. No panel or
param changes; the existing MELODY (VARIATION) knob gains staged depth.

## Problem

VARIATION currently evolves only pitch: GROW mutates fired-slot pitches, RENEW
regenerates motif units. The groove never changes except via NEW PHRASE or a
step-count change. Users want the rhythm to drift *slightly* — but rhythm changes
weigh much more perceptually than pitch changes, so tying them 1:1 to the same dice
would get busy fast.

## Decision (user design)

Split each half of the knob into two zones by `a = |variation|`:

- **Zone 1 (`a` in 0…0.25): melody only.** Exactly today's behavior. Groove frozen.
- **Zone 2 (`a` in 0.25…1.0): rhythm increasingly joins.** A second, groove-specific
  dice ramps in: `r = clamp((a − 0.25) / 0.75, 0, 1)`, probability per cycle wrap
  `∝ r²` (same square law as pitch, fine control at the zone edge).

At most **one rhythm mutation attempt per wrap** (unlike pitch-GROW's per-step dice):
rhythm drifts, melody lives.

## Mutations

All operate on the lane's `GrooveCell` in place, at a cycle wrap only, melodic STEP
lanes only. Every mutation preserves the invariants: `rank_of_slot` stays a
permutation of `0..L−1`, **slot 0 keeps rank 0** (anchor unmaskable, DENSE
monotonicity survives every mutation), `note_len` stays in `[1,4]`.

### GROW side (variation > 0) — drift

On dice success, 50/50 (one draw):

- **Adjacent rank swap:** pick `j` uniform in `1..L−2` (excludes rank 0), swap the
  two slots holding ranks `j` and `j+1`. One note moves one place in the order DENSE
  reveals notes — the audible pattern at a fixed mid DENSE changes by at most one
  note in/one note out.
- **Length nudge:** pick a slot uniform in `0..L−1`, `note_len ± 1` (direction by
  draw), clamped to `[1,4]`.

`L < 4`: no valid swap positions → the swap branch is a no-op (dice and selector
draws still consumed as drawn).

### RENEW side (variation < 0) — re-decide

On dice success, 70/30 (one draw):

- **Push flip (70%):** pick an even candidate `s` uniform from `{2, 4, …} ∩ [2, L−2]`,
  swap `rank_of_slot[s−1] ↔ rank_of_slot[s]`. This is the exact semantic toggle of
  the groove generator's push displacement: if the off-beat outranked its beat, the
  beat is back on the grid; if not, it becomes an anticipation. No score state
  needed — the flip IS a rank swap. `L < 4`: no candidates → no-op.
- **Length nudge (30%):** as in GROW.
- **Full re-roll (extreme only):** when `a ≥ 0.9`, before the flip/nudge selector,
  one extra draw: with probability `kGrooveRerollProb` (≈0.25, ear-tunable) the whole
  cell regenerates via `pg_gen_groove(rng, L, cell)` instead — the RENEW-side analog
  of "at −1, units renew every cycle."

## Placement & determinism

In `ModLane::process()`'s wrap block, per side, *after* the existing draws
(GROW: after the three EVOLVE-walk draws; RENEW: after `_renew_units()` /
`_renew_walk()` and the walk decay), guarded by `_melodic && _step_mode`:

1. one dice draw (always drawn when the side is active — fixed base draw count),
2. on success, the mutation's own draws (selector, position, direction — conditional,
   like `regenerate_unit`'s draws today).

Draw order is documented in code. All draws through the lane `_rng`; two
identically-driven lanes stay bit-identical, including mutations. Non-melodic lanes
and FLOW mode: untouched (no groove). LOOP (`variation = 0`): everything frozen, as
today. NEW PHRASE: still re-rolls pitch + groove together.

Currently-sounding notes are unaffected mid-flight: `_note_hold` was computed at
fire time; mutated ranks/lengths take effect from the next fire on.

## API

New header functions in `phrase_gen.h` (unit-testable, no lane state):

```cpp
void pg_groove_mutate_grow(Rng& rng, GrooveCell& g);              // swap-or-nudge
void pg_groove_mutate_renew(Rng& rng, GrooveCell& g, bool reroll); // flip-or-nudge, or full re-roll
```

The lane owns zoning + dice: `kGrooveVarStart = 0.25f`, dice `r²`,
`kGrooveRerollGate = 0.9f`, `kGrooveRerollProb ≈ 0.25f` (constants in lane.cpp,
ear-tunable; the spec fixes behavior, not values).

## Testing

Unit (`test_phrase_gen.cpp`):
1. Both mutators preserve: permutation, anchor rank 0, lengths in `[1,4]` — over many
   seeds and L ∈ {1, 2, 7, 8}.
2. `pg_groove_mutate_grow` changes at most one thing: either nothing (legal no-op at
   small L), or ranks differ in exactly two adjacent-rank positions, or exactly one
   length differs by 1 — never more than one of these.
3. Push flip: the two swapped ranks belong to slots `s−1, s` with `s` even, `s ≤ L−2`.
4. Determinism: same seed → same mutation.

Lane (`test_gate_density.cpp` or `test_variation.cpp`):
5. **Zone 1 contract:** variation +0.2 and −0.2 → fired step set identical across
   many cycles (pitch may drift; rhythm must not).
6. **Zone 2 acts:** variation +1.0 → fired step set at fixed mid DENSE differs
   between early and late cycles (fixed seed, deterministic); same for −1.0.
7. LOOP freeze and identical-drive bit-determinism still hold (existing tests).

## Non-goals (YAGNI)

- Independent rhythm-variation control.
- Mutating the groove of non-melodic lanes.
- Zone boundaries as user parameters.
- Panel/tooltip changes.
