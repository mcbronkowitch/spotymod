# Ranked-Slot Groove — Rhythm for the Melody Engine

**Date:** 2026-07-16
**Status:** Approved design, pre-implementation
**Scope:** PITCH lane only (`phrase_gen.h`, `lane.h/.cpp`, tests). No panel or param changes.

## Problem

The melody engine composes pitch (motifs, A A B A form, call/response, renewal) but
rhythm is a side-effect of `pg_metric_weight()`: notes can only land on strong binary
positions, and DENSE only removes weak positions first. The result is a metronomic
4th/8th feel. Syncopation is structurally impossible (off-beats die first, never lead),
notes have no composed durations, and rhythm carries no repeating identity the way
pitch motifs do.

## Decisions (from brainstorm)

1. **Grid-bound**: rhythm = *which* steps fire. Notes stay exactly on step boundaries.
   No swing, no microtiming.
2. **DENSE is the rhythm knob**: it morphs through the groove (sparse syncopated
   anchors → groovy pattern → running steps). No new controls.
3. **One groove per phrase**: all motif instances (A and B) share one rhythm pattern,
   period = motif length `L`, tiled across the phrase. Rhythm unifies; pitch
   differentiates.

## Design

### 1. Groove table (phrase_gen)

`generate_phrase()` additionally composes one **groove cell** for the phrase:
a ranking `rank_of_slot[L]` (a permutation of `0..L-1`) plus a per-slot note length
`note_len[L]`.

Construction (deterministic, all draws via the caller's `Rng`, fixed draw order):

1. Score each slot with its metric weight `pg_metric_weight(i)` (motif-relative).
2. Roll a seeded **syncopation degree** for the phrase (range mild → spicy; exact
   bounds tuned by ear).
3. **Displacement (pushes):** for each strong slot `s` (`tz ≥ 1`, excluding slot 0),
   with probability derived from the syncopation degree, move the emphasis to the
   off-beat before it: `score[s-1] = score[s] + bonus`, and demote `score[s]`.
   Slot `L-1` pushing "into slot L" anticipates the next cell's downbeat — valid and
   desirable under tiling.
4. Add small seeded jitter to all scores (tie-breaking variety between phrases).
5. Sort slots by score descending → `rank_of_slot`. **Invariant: slot 0 is rank 0**
   (the downbeat anchor is always the first note to exist). This is *enforced*, not
   emergent: slot 0's score is pinned above any reachable pushed score (a push bonus
   could otherwise outrank the downbeat).
6. For each slot, draw `note_len[i] ∈ 1..4` (distribution tuned by ear). Effective
   length at play time is additionally capped by the distance to the next *active*
   note (see §3).

The tail (`r` slots) uses the same cell truncated to `r`.

`pg_gen_motif()` no longer composes gates; its `gate[]` output becomes all-true.
(The arrays stay so `regenerate_unit` and buffer layout are untouched.)

### 2. DENSE = ranking depth (lane)

```
k = max(1, round(density * L))
effective_gate(slot) = rank_of_slot[slot % L] < k
```

This **replaces** both the old metric-weight gate in `pg_gen_motif` and
`_density_pass` — one mechanism instead of two.

Properties:

- **Monotonic and stable**: raising DENSE only *adds* notes to the same groove, in
  composed order; lowering removes them in reverse. The pattern never re-rolls under
  the knob.
- density → 0: one note per cell (the anchor). density → 1: every step fires.
- Non-melodic lanes ignore all of this (DENSE feeds only `LANE_PITCH` via
  `SuperModulator::set_density`); their gates stay all-true as today.

### 3. Durations on the GATE output (lane)

Today `gate_state()` mirrors the current slot's gate, so every note is exactly as long
as its run of gated slots. New behavior: the lane tracks the current note's start slot
and length. When a note fires at slot `s`:

```
len = min(note_len[s % L], distance to next active slot)
```

`gate_state()` returns "current position is within the sustained note". A note whose
length reaches the next active note ties into it (legato, gate stays high — current
behavior for runs); a shorter length drops the gate early, leaving a real rest. Pitch
S&H behavior is unchanged (held value freezes through rests as today).

### 4. Lifecycle

- **NEW PHRASE / step-count change**: groove and pitch re-roll together (same
  `_regen_pending` path).
- **RENEW** (variation < 0): regenerates pitch units only. The groove is the phrase's
  identity; pitch evolves over a constant rhythm.
- **GROW** (variation > 0): unchanged (pitch mutation + EVOLVE walks).
- **FLOW mode, SYNC/COUPLE, non-melodic lanes**: untouched.

## Error handling / edge cases

- `L = 1`: ranking is trivially `[0]`; density has one level; behaves like today.
- `k` clamped to `[1, L]`; density NaN/out-of-range already clamped by `set_density`.
- Determinism invariant holds: no heap, no time, all randomness through the lane
  `Rng` in a fixed order (groove drawn after pitch content in `generate_phrase`;
  document the order in code so tests can rely on it).

## Testing

Unit tests in `tests/test_phrase_gen.cpp` (+ lane tests where noted):

1. Same seed → identical `rank_of_slot`, `note_len` (reproducibility).
2. `rank_of_slot` is a valid permutation; slot 0 has rank 0.
3. **Monotonicity**: for k' > k, the gated set at k is a subset of the gated set
   at k' (raising DENSE never moves/removes existing notes).
4. Note lengths within `[1, 4]` and capped by next-active-note distance (lane test).
5. Tiling: every instance and the tail read the same cell pattern.
6. Statistical (over many seeds, mid density): a nonzero fraction of phrases gate at
   least one odd (off-beat) slot — syncopation actually occurs.
7. Existing gate-expectation tests updated (old metric-threshold gates are gone).

Listening pass: render `host/render/scenarios/demo_step_melody.json` before/after;
audition DENSE sweeps at 8 and 16 steps in Rack.

## Non-goals (YAGNI)

- Swing / microtiming.
- Per-motif or per-principle rhythm vocabularies.
- Accent/velocity output.
- Any new panel control.

## Tuning constants (by ear, not spec-fixed)

Syncopation-degree range, push probability curve, demotion amount, jitter width,
`note_len` distribution. The spec fixes *behavior* (monotonic ranking, anchor-first,
push displacement, tie/rest semantics); constants live in code, tuned during the
listening pass — matching the engine's existing convention.
