# Part glue to control rate — stop computing what nobody reads

**Date:** 2026-07-19
**Status:** approved (brainstorm with Bastian, 2026-07-19)
**Context:** the ablation family (`docs/bench/2026-07-19-9be5df9.md`, plan
`2026-07-18-bench-ablation-family.md`) closed the instrument's unaccounted
39 % and named **Part glue** the single largest attributed line:
**112 820 cycles per part** (≈12 % of the 960 000-cycle block budget each,
≈23 % for both). `instrument_worst` sits at 150.83 % avg / 156.06 % max
offline and 152.03 % / 155.95 % anchored. This spec is the first cut written
against those figures.

It does not, on its own, reach the goal. Under 100 % additionally needs the
reverb item and the `EchoDelay` fast-tanh item from the same ranked list.

## The finding this spec rests on

`SynthEngine` already consumes its targets at control rate.
`engine/synth/synth_engine.cpp:247` decrements `_ctrl_ctr` and calls
`_update_control()` every `kCtrlInterval = 96` samples; that function reads
`_targets[LANE_SOURCE]`, `_targets[LANE_SIZE]`, `_targets[LANE_MOTION]` and
the chord pitches. `Part::process()` computes them 96 times per block. **95
of the 96 results are discarded.**

So this is not primarily an approximation trade. It is removing redundant
work, and that is why the cut list's 23 % ceiling is not inherited here: it
assumes the glue goes to zero, when in fact a per-sample remainder is
mandatory.

Three consumer classes set the raster boundary:

| Path | Read at | Rasterable |
|---|---|---|
| `set_targets` SOURCE/SIZE/MOTION, `set_chord` | control tick | yes — phase-aligned, even bit-identically |
| `_targets[LANE_LEVEL]` | **per sample** (`_level.process(...)`, `synth_engine.cpp:251`) | only with an audible delta |
| `fxv[5]` | **per sample** (`PartFx::_smooth[i].process`, `part_fx.cpp:27`) | only with an audible delta |
| chord build on a fire | **event** | no — needs an event refresh |

## Not a contradiction of the mod-plane spec

`2026-07-18-mod-plane-optimization-design.md` ruled control-rate rework out
of scope, because STEP must stay sample-accurate and `SMOOTH = 0`'s
near-passthrough slew would become audibly steppy. That ruling is about the
**lanes**. This spec does not touch `ModLane` or `SuperModulator::process()`
— lane outputs keep being produced every sample. Only their **consumption
inside `Part::process`** moves onto a raster, and only for consumers that
already read at that raster.

The `SMOOTH = 0` risk case largely dissolves for the same reason: the fast
intermediate values were never seen by the engine. What genuinely remains is
fire timing, and that is answered with an event, not a finer raster.

## Decisions (from brainstorming, 2026-07-19)

- **The engine's output may change.** The instrument is pre-release; no
  compatibility constraint applies. Bit-identity is used below as a *test
  instrument*, never as a product promise.
- **The raster belongs to `Part`.** Revised during planning — see below.
- **The fire path stays event-driven** even though nothing forces it. A
  pitch frozen at the control tick is not "2 ms late" under FLOW with
  `SMOOTH = 0` — it is the wrong note, because the lane's step lands exactly
  on the fire. Fires are rare; the refresh is free.
- **Two steps with a gate between them**, one spec, two commits.
- **Success is a measured reduction**, from the `part_glue_flow` row before
  and after — not the estimate below.

## Architecture

### Raster ownership

**`Part` owns the counter.** The brainstorm settled on `Instrument` owning
it; planning overturned that, and the reason is worth recording.

`Instrument` cannot own it. 35 test cases across `test_part.cpp`,
`test_choke.cpp`, `test_mod_tide.cpp` and `test_center.cpp` construct a
`Part` standalone and call `process()` directly, with no `Instrument` in
sight. The `part_glue_flow` bench row does the same. A tick owned by
`Instrument` leaves every one of those callers with targets that never
update.

`Part` is also the correct owner on the merits, not merely the workable one.
The counter has to align with **`SynthEngine`'s** tick, and that engine
belongs 1:1 to a `Part`; `Instrument`'s counter drives `Center`, a different
axis. A `Part`-owned counter is phase-aligned by construction, because
`_engine->process()` runs exactly once per `Part::process()`. Both init to
0 and fire on samples 0, 96, 192 — `SynthEngine` tests `--_ctrl_ctr <= 0`
(`synth_engine.cpp:247`), `Part` tests `== 0` before decrementing. Within a
tick sample the order is: `control_tick` (targets computed, `set_chord`
pushed) → `_engine->process()` → engine tick reads. Exactly what the
per-sample code delivers at that sample, which is what makes Step 1's gate
provable.

**Known bounded divergence:** after a `set_engine` swap, `SynthEngine`'s
counter is offset against `Part`'s, because it did not run while that engine
was inactive. The engine then reads a target up to 96 samples stale for one
interval. The engine fade is at zero across that window. Documented, not
fixed.

### Three paths through `Part::process`

**Raster** (`control_tick`): `target_raw` for SOURCE / SIZE / MOTION /
PITCH, `pitch_pre_quant` + `_quant.process`, `_color_eff`,
`_chord.set_color`, `_chord.apply`, `_engine->set_chord`. Results land in a
`_targets[]` cache owned by `Part`.

The duplicated PITCH evaluation — `target_raw(LANE_PITCH)` runs once in the
targets loop and again inside `pitch_pre_quant()` — disappears here as a
by-product. That is item 6 of the cut list, absorbed at no extra cost.

**Per sample** (Step 1): `_engine_fade.process()`, the `master_hz` compare,
both `lane_fired` reads, `_gate_ctr`, `target_raw(LANE_LEVEL)` written into
the cache, and the `set_targets` push from the cache.

The push deliberately stays per-sample in Step 1. It costs five copies and
one virtual call, and keeping it avoids adding a `set_level` member to
`IPartEngine` purely to prop up a temporary gate. Step 2 removes it.

**Event**: the `_mod.lane_fired(LANE_PITCH)` branch recomputes the pitch and
runs `_chord.build` fresh, outside the raster.

### Quantizer slew

`Quantizer::init` sets `_slew_len = sample_rate * 0.04f` — a sample count.
Called 1-in-96, its 40 ms change slew would become 3.8 s. `_slew_len`
converts to control ticks: `sample_rate * 0.04f / kCtrlInterval`, floored at
1 so it cannot round to zero.

This is the one place where the raster silently changes an existing
time constant. There is no other sample-counting state in the rasterized
path — `_gate_ctr` and `_gate_len` stay on the per-sample side.

## Step 1 — the raster, with an identity gate

Everything above except the Step 2 list. The gate is what makes this worth
splitting out: the refactor is not trivial, and a step that *must* come out
identical is the cheapest correctness proof this repo has.

**Bit-identity is conditional, and the condition is named.** Two pieces of
state in the rasterized path are call-cadence dependent:

- `Quantizer::process` holds `_last_note` with `HYST_SEMIS = 0.30`. "Hold
  the previous note" means the previous *tick*'s note instead of the
  previous *sample*'s. When the PITCH lane crosses a scale boundary, the
  decision can flip.
- `ChordBuilder::set_color` holds zone hysteresis (`_above2..4`, `_ninth`,
  `kHyst = 0.02`) with the same cadence dependence.

So the gate is two renders, not one:

1. **Identity render** — `QuantMode::Free` (the quantizer is a stateless
   passthrough there, `quantizer.h:57`), COLOR fixed away from a zone edge,
   the MOTION→COLOR path inactive, PITCH lane moving. Structurally
   hysteresis-free, therefore **must be byte-identical**. This proves the
   refactor.
2. **Default render** — may differ. The difference must be attributable to
   the two hysteresis paths: same note grid, occasional different edge
   resolution. Anything else is a bug, not the design.

## Step 2 — the rest onto the raster

`target_raw(LANE_LEVEL)`, the five `fx_target_value` calls, and the
`set_targets` push itself move onto the tick. `PartFx`'s 2 ms smoothers and
the engine's 10 ms `_level` absorb the steps.

The render changes audibly from here. Because Step 1 was proven clean, the
measured delta is attributable to these six items rather than to a mistake
in the restructuring.

## Expected saving — derived here, not inherited

The cut list's ≈23 % ceiling assumes the glue reaches zero. Measured, the
glue is 112 820 cycles per part over 96 samples = **≈1 175 cycles per
sample**.

After Step 1 the per-sample remainder is: the fade smoother, two
`lane_fired` reads, the gate counter, one `target_raw`, five
`fx_target_value`, and the push. After Step 2: fade, fires, gate, and
whatever the push leaves behind.

Estimate: **70–85 % of the glue, so ≈16–19 % of budget across both parts** —
below the 23 % ceiling, as expected once the mandatory remainder is
acknowledged.

**The assumption this estimate can fail on:** it treats `Quantizer::process`
— with its `nearest_note` outward search and the `% 12` in `allowed()` — as
the dominant glue item. No bench row proves that. If the real distribution
is flatter, the five `fx_target_value` calls left standing in Step 1 are a
larger share than assumed and Step 1 under-delivers while Step 2
over-delivers. The re-measured `part_glue_flow` row settles it; this
arithmetic does not.

## Verification

| What | How |
|---|---|
| Step 1 correctness | identity render byte-identical; default render diff explained by the two hysteresis paths |
| Step 1 + 2 yield | `part_glue_flow` before/after — the row exists and is the measurement |
| Budget effect | `instrument_worst` offline + anchored |
| No regression elsewhere | existing unit-test suite |
| Step 2 audible cost | listening pass |

`part_glue_flow` runs its `Part` standalone, outside `Instrument`. Because
the counter lives in `Part`, the row needs no harness change and measures
the real rastered cost — this was the deciding argument for the ownership
revision above.

The bench run needs the Daisy Seed attached (`python bench/run.py`), so it
is a human step, not something the implementation tasks can self-serve.

## Out of scope

- `ModLane` / `SuperModulator` internals — the mod-plane spec owns those and
  its ruling stands.
- The reverb coupling item, `EchoDelay` fast tanh, the limiter's `shape()`,
  and `PartFx`'s rev-send `std::sin` — separate specs from the same list.
- Voice count and the 2×4 verdict. This cut does not change it.
- `PartFx`'s and the engine's own per-sample smoothers. Now that identity is
  not a product constraint they are legitimate targets, but they are FX and
  engine rows, not glue.
