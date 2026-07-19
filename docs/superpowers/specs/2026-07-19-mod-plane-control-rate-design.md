# Mod plane to control rate — texture lanes tick on the 96-sample raster

**Date:** 2026-07-19
**Status:** Approved (design review with user; approach C of three)
**Depends on:** part-glue control-rate rework (merged on `cpu-hunt`, spec
`2026-07-19-part-glue-control-rate-design.md`)

## Context

The hardware bench (`docs/bench/2026-07-19-c7f6a73.md`) puts `instrument_worst`
at **117 % avg / 123 % max** of the 960k-cycle block budget — still over — and
`mod_plane_2x_center` at **26.4 %**: ten `ModLane`s advanced every sample plus
the Center tick.

Since the part-glue rework, **nothing in the audio path reads a texture lane
between control ticks.** `Part::_control_tick()` samples `lane_output()` once
per `SynthEngine::kCtrlInterval` (96) samples; Center reads only
`pitch_phase()` at its own 96-sample tick; `pitch_cv()` is the tick-cached
quantizer output. The per-sample computation of the four texture lanes per part
(SOURCE/SIZE/MOTION/LEVEL) is therefore pure waste: 96 values computed, 1 read.
The old mod-plane spec's out-of-scope ruling on control-rate lanes predates
that fact and is superseded here.

The one remaining sample-accurate observable is the **PITCH-lane fire**
(`Part::process` reads `lane_fired(LANE_PITCH)` every sample → note trigger +
gate pulse). **User decision (2026-07-19): note timing stays sample-accurate.**
At fast rates (1/32 @ 120 BPM ≈ 7.8 ms steps, COUPLE-scaled down to ~100
samples/step) a ±2 ms onset raster would audibly smear the groove engine's
push/anticipation timing.

## Decision

**Approach C: the four texture lanes per part advance on the 96-sample raster;
the PITCH lane keeps its per-sample path unchanged.**

Rejected alternatives:

- **A — quantize everything to the raster** (fires land on ticks): simplest and
  cheapest, but violates the sample-accurate-timing decision above.
- **B — all lanes on the raster + analytic fire scheduling for PITCH**: the
  lane predicts the exact boundary sample at each tick (exact by construction —
  all rate changes land on Center's tick, phase is linear inside an interval)
  and `Part` fires on a countdown. ~4–5 more points than C, but the complexity
  lands on the musically critical path (trigger, fire refresh, gate). **Named
  follow-up, not part of this spec.** C's tick machinery is B's foundation;
  nothing built here is thrown away if B is needed later.

Expected saving: **~17–19 points** of block budget (4 of 5 lanes × 2 parts).
Budget arithmetic after this + the separate FLUX-tanh cut: ≈ 97 % max, with B
and the voice cap (~9 pts) in reserve.

## Design

### Two advance paths, both permanent

`ModLane` keeps `process()` (advance one sample) **unchanged** and gains
`tick()` (advance exactly `kTickInterval` = 96 samples in one call). Both paths
stay in the binary permanently: PITCH uses `process()`, texture lanes use
`tick()`, and the equivalence test suite (below) drives both against each
other forever.

The interval is a `ModLane` constant. The mod layer must not include synth
headers; `part.cpp` pins the alignment with
`static_assert(ModLane::kTickInterval == SynthEngine::kCtrlInterval)`.

### Driving the lanes (`SuperModulator::process`)

Called once per sample, as today:

- `_out[LANE_PITCH] = _lanes[LANE_PITCH].process();` — every sample, untouched.
- Texture lanes: an internal counter, initialized so the **first** `process()`
  call ticks, runs `_out[i] = _lanes[i].tick()` for the four texture lanes once
  per 96 calls.

Because `Part::_ctrl_ctr` also fires on the first `process()` of a run and
`_mod.process()` runs before the raster branch inside `Part::process`, the mod
tick lands on the same samples as `Part::_control_tick()` and executes first
within that sample — `_control_tick` always reads texture values that are 0
samples old, exactly as fresh as today.

**Accepted asymmetry:** an engine swap re-arms `Part::_ctrl_ctr` to 0
mid-grid; the SuperModulator counter does not follow. Texture values are then
up to 95 samples (~2 ms) stale at the read until the grids happen to
coincide — the same class as the documented SynthEngine `_ctrl_ctr` offset
asymmetry in `part.h`, and accepted for the same reason (rare knob gesture,
musically negligible).

### `tick()` semantics

`tick()` must produce the same *observable sequence* as 96 consecutive
`process()` calls, with output sampled at the tick only:

1. **Phase:** one fused advance,
   `_phase += _phase_inc * (1 + _ev_rate) * kTickInterval`. Same end phase as
   96 single adds modulo float rounding (a fused add rounds *less*); the
   difference is accepted — renders are not byte-compared across this change.
2. **Boundary loop:** every step boundary crossed inside the interval is
   processed **individually, in phase order** — `_cur_step` walks each crossed
   step and `_on_boundary()` runs per step, so RNG draws (`_mutate_slot` dice),
   note aging (`_note_age`), holds and freezes happen exactly as today. Each
   lane owns its RNG stream, so per-lane determinism is preserved by
   construction. Only the **last** boundary's `_target` becomes visible; the
   skipped intermediates lived < 2 ms and were never read since part-glue.
   Worst case boundary count: rate 30 Hz × ratio 2 × TIDE 4 × COUPLE clamp 2 =
   480 Hz effective → ~12.5 samples/step in STEP mode → up to 8 boundaries per
   tick. The loop is hard-capped at `2 * kSeqSlots` (64) iterations as a
   safety bound; the cap is unreachable from the panel.
3. **Wrap ordering:** if the interval crosses phase 1.0, the wrap events
   (pending regen, EVOLVE walk, `_mutate_groove`) run at their phase position:
   after the boundaries before the wrap, before step 0 of the new cycle. This
   matches the per-sample order (wrap block runs before step detection within
   the wrap sample).
4. **Decays and glides:** SPOT kick decay and SETTLE glide use precomputed
   per-tick coefficients (`coef^96`); `_settle_ctr` counts ticks. The SMOOTH
   `OnePole` slews once per tick with a coefficient initialized at tick rate
   (the `Quantizer::init(sr, interval)` precedent). The lane precomputes both
   the per-sample and per-tick variants in `init`/`_update_slew`; `process()`
   and `tick()` each apply their own. Whether that is two coefficients on one
   `OnePole` or two instances is a plan-level choice; behavior is what is
   specified.
5. **FLOW:** `_compute_raw()` runs once at the final phase instead of 96
   times. This is where the bulk of the saving lives (`shape_value` is the
   expensive kernel). The value equals what today's per-sample path would show
   at the same read sample (same end phase).
6. **Flags:** `fired()` / `frozen()` report whether *any* boundary in the
   interval fired / the last boundary's freeze state. Sole consumer of texture
   `fired()` is the VCV LED boost (UI rate) — tick-wide flags make the LEDs
   slightly *more* visible, no other consumer exists.
7. `kick()`, `settle()`, `reset()`, `set_step()` live-rescale operate on
   stored state and need no semantic change. SPOT/servo/TIDE rate changes all
   land on Center's control tick, which shares the 96-grid — a kick applied at
   that sample is absorbed by the same tick that today absorbs it over 96
   single steps, at the same phase positions.

### What stays bit-identical, what changes

**Bit-identical:** every PITCH fire sample, gate pulse, `pitch_cv()`, groove
state, CHOKE window, Center servo behavior (reads `pitch_phase()` only), the
whole melodic test surface. No API, param, or panel change; the VCV plugin
needs only a rebuild.

**Changes:** texture-lane outputs differ microscopically (one slew step per
tick instead of 96 with the same time constant — near-identical at the read
points, not byte-identical). Consequences:

- All renders diverge byte-wise. Per project policy
  (bit-exactness-not-required) renders are sanity checks; the `renders/`
  refresh belongs to the user's listening pass.
- `host/render/scenarios/ctrl_identity.sha256` is **re-pinned** after the
  change; the scenario lives on as a same-build double-render determinism
  gate.

## Testing & acceptance

1. **Equivalence suite (the heart of the design):** the same lane config
   (seed, rate, shape, smooth, STEP/FLOW, variation) driven once via 96×
   `process()` and once via `tick()` must show: identical boundary counts,
   exactly equal `target()` values at each boundary, identical `_seq` buffer
   contents after N cycles (proves the RNG streams never diverged), and
   smoothed outputs at tick samples within a small epsilon. Covers STEP, FLOW,
   variation > 0 (GROW dice), variation < 0 (RENEW), and a SPOT kick + SETTLE
   run. These tests are permanent, not scaffolding.
2. **Multi-boundary tick:** an extreme rate forcing ≥ 3 boundaries per tick —
   every slot visited in order, note aging and holds correct, loop cap never
   hit at panel-reachable rates.
3. **Wrap ordering:** a pending regen lands before step 0 of the new cycle,
   matching the per-sample reference run.
4. **Hardest gate: no existing PITCH / groove / CHOKE / center test may be
   edited.** If a melodic-path test needs touching, the design is violated —
   stop and reassess.
5. **Determinism:** double render of at least two scenarios byte-identical.
6. **Measured saving:** hardware bench re-run (user step) — expected
   `mod_plane_2x_center` ≈ 26 % → single-digit, `instrument_worst` −17 to −19
   points. Estimates are not trusted until measured.

## Non-goals

- Approach B (analytic fire scheduling, PITCH lane on the raster) — named
  follow-up if the budget stays tight after this + the FLUX cut.
- The FLUX feedback-cap/tanh cut, voice cap, Svf replacement — separate specs.
- Any behavior or feature change beyond the smoothing granularity described
  above. This is a cost rework, not a redesign.
