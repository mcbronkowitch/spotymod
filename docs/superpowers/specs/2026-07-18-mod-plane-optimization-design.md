# Mod-plane optimization — measure the parts, then cut

**Date:** 2026-07-18
**Status:** approved (brainstorm with Bastian, 2026-07-18)
**Context:** the bench firmware (`docs/bench/2026-07-18-7e99b74.md`, spec
`2026-07-18-bench-firmware-design.md`) measured the modulation plane at
**33 % of the block budget** against the modulation-first design spec's
estimate of 4–6 % — wrong by roughly six times, and the single most
actionable finding in the table. It is also a conservative figure: the
bench's modulators sat in zero-wait DTCMRAM while the firmware runs them
from AXI SRAM inside `Instrument`.

The same run put the full instrument's worst case at 164 % of budget, so
the plane matters. It does not, on its own, fix that: zeroing the entire
mod plane still leaves ~132 %. This spec covers one step of several.

## Decisions (from brainstorming, 2026-07-18)

- **The mod plane's output may change.** `fast_sin` in place of libm is
  allowed; reference renders get re-cut and their byte-identity stops
  being a regression gate. The audio path has done exactly this since M2
  (`engine/util/fast_sin.h`), so the engine ends up consistent with
  itself rather than diverging.
- **Scope is the mod plane only** — `ModLane`, `SuperModulator`, `Center`.
  Not FLUX, not the reverb, not voice count. Those are separate specs.
- **Success is a measured reduction**, before and after, from bench rows —
  not a target number. We do not yet know what is achievable, and a
  guessed target would be one more unfounded estimate in a project whose
  whole point was removing them.
- **Measure the decomposition before cutting.** One number for the whole
  plane cannot say where the cycles go.

## The prediction this spec can fail on

`engine/mod/waveforms.h:7` is `wave_sine(ph) = std::sin(ph * TWO_PI)`.
`shape_value()` runs per sample per lane, and reaches `wave_sine` whenever
SHAPE < 0.25 — which includes `ModLane::_shape`'s default of `0.f`, so the
33 % figure was measured **with** the sine in the path.

- Measured: 315 673 cycles / 96 samples = **3 288 cycles per sample**
- Ten lanes (`LANE_COUNT = 5` × 2 parts) × ~170 cycles for `sinf`
  (measured in the bench's own `sinf_x96` row) = **~1 700 cycles per sample**
- So **~52 % of the mod plane should be the sine alone**

`fast_sin.h` documents ~10–15 cycles on the M7 against ~80–120 for libm
sinf. If the prediction holds, the plane falls from 33 % to roughly
**17 %** on that one change.

**If Phase 1 contradicts this, that is the finding** — record it and
re-plan Phase 2 from the real numbers rather than pursuing the sine.

## Phase 1 — decomposition rows

A new bench family `mod`, added to `bench/workloads_*.cpp` as one table row
each. The existing `mod_plane_2x_center` row stays as the roll-up so the
parts can be checked against the whole.

| Row | Answers |
|-----|---------|
| `lane_flow_shape0` | one `ModLane`, FLOW, SHAPE 0 — the sine segment |
| `lane_flow_shape3` | SHAPE 0.3 — triangle→ramp, no sine |
| `lane_flow_shape7` | SHAPE 0.7 — ramp→pulse, no sine |
| `lane_flow_shape10` | SHAPE 1.0 — pure S&H, no sine |
| `lane_step` | one lane in STEP: does `_compute_raw` really run only on a fire? |
| `super_mod_1` | one `SuperModulator` (5 lanes) — is there overhead above the lanes? |
| `center_tick` | `Center::update` alone, at its real control-rate cadence |

Three questions nobody can currently answer: what one lane costs and how
strongly that depends on SHAPE; whether `SuperModulator` adds cost above
its five lanes; and what share `Center` actually holds, given it runs once
per 96 samples rather than per sample.

**Every row must measure what its name says.** This project shipped two
rows that did not — a "1 voice" row measuring 2.8 voices, and an "anchor"
row 7.3× cheaper than the voice it claimed to represent. Both produced
confidently wrong numbers. Assert the lane's mode and shape rather than
assuming the setter took effect.

## Phase 2 — the cuts, in order

1. **`wave_sine` → `fast_sin`.** One line. The prediction above rides on
   it, so it is measured on its own before anything else changes.
2. **Whatever Phase 1 ranks next.** Candidates, none yet confirmed:
   - `std::floor` in `_compute_raw` (`lane.cpp:128`) — cheap on the M7,
     but it runs ten times per sample.
   - The SPOT/SETTLE decay multiplies at the top of `ModLane::process`,
     which run every sample whether or not anything kicked.
   - Whether `_compute_raw` is genuinely fire-gated in STEP.

Each change lands separately with before/after from the same rows. A
change that does not move a number gets reverted, not kept for tidiness.

### What the implementation plan can and cannot cover

Phase 1 and cut 1 are fully specifiable today and belong in one plan.
Cut 2 is not: it depends on numbers that do not exist yet, and a plan step
reading "optimise whatever turns out to be expensive" is a placeholder
wearing a task's clothes.

So the plan for this spec ends at a **decision checkpoint** — Phase 1's
rows landed, `fast_sin` landed and measured, the prediction confirmed or
refuted. Whatever comes after gets its own spec and plan, written against
real figures. If the sine turns out to be the whole story, there may be
nothing after, and that is a fine outcome.

## Acceptance

1. The `mod` family rows exist, and two consecutive runs are
   checksum-identical, as with every other family.
2. `mod_plane_2x_center` is measurably lower, with the before figure
   (33 %) and the after figure both recorded in a committed
   `docs/bench/` result file.
3. The prediction above is explicitly confirmed or refuted in writing.
4. The unit tests under `tests/` pass unchanged. They cover lane
   behaviour, gate density, step-clock and variation — structure, not
   exact bits — so they are the right net for this change.

## Error behaviour and risks

- **`fast_sin`'s error is < 1.2e-3.** Inaudible on an LFO, but S&H and
  gate decisions sit on thresholds, so individual triggers can shift by a
  sample. The unit tests catch structural breaks, not shifts of this kind.
- **Reference renders under `renders/` get re-cut.** Their byte-identity
  stops being a gate. This is the accepted price of the decision above,
  not an oversight.
- If a re-cut scenario sounds meaningfully different, that is an
  observation for Bastian's ear, not automatically a regression.

## Out of scope (YAGNI)

- **No control-rate rework of the lanes.** Computing lane values every
  8–16 samples and interpolating through the existing `_slew` could be
  worth a factor of 8–16, but STEP firing must stay sample-accurate and
  at SMOOTH 0 the slew is near-passthrough, where stepping would become
  audible. Revisit only if Phase 1's numbers justify the risk.
- No FLUX, reverb, or voice-count work — separate specs.
- No change to lane semantics: rates, ratios, gate logic, phrase
  generation and groove all stay as they are.
- No new panel control or parameter id.

## Assumptions to verify during implementation

- That `_compute_raw` runs per sample in FLOW and only on a fire in STEP.
  The `lane_step` row exists to check it.
- That `SuperModulator::process` adds little above its five lanes.
- That `Center`'s control-rate cadence really amortises its transcendental
  load (`center.cpp` carries many `pow`/`sin`/`sqrt` calls, but at 1/96
  the rate).
- That the bench's DTCMRAM placement of the modulators, which makes the
  33 % figure conservative, does not also mask the relative ranking of
  the parts.
