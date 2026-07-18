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

## Outcome (2026-07-18)

**Prediction, as written above:** ten lanes × ~170 cycles for `sinf` ≈
1 700 cycles per sample ≈ 52 % of the plane's per-sample cost, so
`mod_plane_2x_center` should fall from about 33 % to about 17 % once
`wave_sine` used `fast_sin`.

**It fell short.** `wave_sine` → `fast_sin` landed (commit `185c488`,
verbatim per Phase 2 cut 1) and `mod_plane_2x_center` fell from 314 713 to
253 084 cycles — about 33 % to about **26 %** of the 960 000-cycle block
budget, not to ~17 %. The realized saving is about 6 points of block
budget, roughly 40 % of the 16 points predicted. The reasoning behind the
extrapolation held up well — all ten lanes do default to SHAPE 0 (the
sine segment) as assumed, and 10 × the measured single-lane saving (6 292
cycles) ≈ 62 920 cycles lines up with the plane's actual saving (61 629
cycles) within 2 %. What was wrong was the per-call cost estimate: the
real saving from swapping libm `sinf` for `fast_sin` is about 66 cycles
per lane per sample, well under half of what the spec assumed.

**Decomposition ranking, post-change** (`docs/bench/2026-07-18-185c488.md` (superseded pair, removed under the one-pair convention; retrievable at commit `a3f8a35`),
`mod` family):

1. The ten lanes account for essentially the whole plane: two
   `super_mod_5lanes` banks (126 916 cycles each) ≈ 253 832 cycles, against
   `mod_plane_2x_center`'s 253 084 — a ~0.3 % gap, consistent with the
   density-setting difference already documented in the Phase 1 decomposition
   check, not a measurement mismatch.
2. Within one bank, a default (SHAPE 0, FLOW) lane is still the single most
   expensive lane at 25 491 cycles (about 3 % of budget). A residual ~4 907-cycle
   gap over SHAPE 0.3 (20 584 cycles) remains — evidence that `fast_sin`
   itself costs closer to ~50 cycles at this call site than the ~10–15
   cycles its own header comment claims, not a coding error.
3. STEP mode stays dramatically cheaper than FLOW at the same shape (11 865
   vs 25 491 cycles, a 53 % cut) — `_compute_raw` really is fire-gated, as
   Phase 1 assumed.
4. `SuperModulator` adds no measurable dispatch overhead above its five
   lanes.
5. `Center::update` is negligible: 4 831 cycles, 0.50 % of budget, once per
   block.

**Next-largest item now** is not a single removable call the way the sine
was: it is the FLOW lane's own per-sample floor — phase advance, slew, and
output composition — which runs regardless of SHAPE and costs 18 664 cycles
even at SHAPE 1.0 (pure S&H, `lane_flow_shape10`, no waveform call at all).
Across ten lanes that floor alone is roughly 186 640 cycles, about 19 % of
the block budget by itself, against the plane's total of 26 % — it is now
the dominant cost, not any one function. The only lever against it is
STEP's existing fire-gating, or the control-rate rework this spec already
ruled out of scope (STEP must stay sample-accurate; SMOOTH 0's near-passthrough
slew would become audibly steppy).

The mod plane's output also changed deliberately as part of this cut —
`fast_sin` is not bit-identical to libm `sin` — so `renders/` byte-identity
is no longer a regression gate for it, per the Decisions section above.

**Recommendation: no follow-up mod-plane spec.** The plane is down to about
26 % of budget from 33 %, and the two levers that would move it further are
both already spent or already out of scope: STEP's fire-gating already
exists and is lane semantics, not a new cut; the residual sine-adjacent cost
is a small, already-explained ~4 900-cycle-per-lane remainder, not another
one-line fix. What is left is the lane state machine's own floor cost, and
cutting that means the control-rate rework this spec explicitly excluded —
a lane-semantics redesign, not a follow-up patch. The instrument's real
problem is elsewhere: `instrument_worst` is still 151 % (avg; 156 % max) of budget offline
with the plane at only 26 % of that total, and zeroing the plane entirely
would still leave the instrument at roughly 125 % — over budget on its own.
Further CPU work belongs in a voice-count or FX spec, where the 2×4 gap
actually lives, not in another cut to the modulation plane.
