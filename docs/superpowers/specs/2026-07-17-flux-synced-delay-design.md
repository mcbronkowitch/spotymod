# FLUX redesign — synced delay + tape-bloom feedback

**Date:** 2026-07-17
**Status:** design approved, ready for implementation plan
**Scope:** the per-part FLUX echo block and its panel/modulation surface (VCV + engine). No change to GRIT, COMP, reverb, or the master chain.

## Problem

FLUX is a per-part tape echo, but today its only dedicated panel control is **MIX**. Its **TIME** and **FEEDBACK** live *only* as modulation-lane targets (`FXT_FLUX_TIME`, `FXT_FLUX_FB`) — you cannot dial the delay time or feedback directly, so the block "makes no sense" to play with. Two concrete gaps:

1. **No tempo relation.** The free-running time (50 ms–5 s exp) never fits the groove.
2. **Feedback too tame.** It reaches 1.1 (110 %) behind a `SoftClip`, but that is neither directly dialable nor a musical, tape-warm self-oscillation.

Additionally, **modulating the delay time makes no musical sense** and must be removed.

The two free slots in the FX knob row (per part) are the room to fix this on real hardware.

## Existing infrastructure this reuses

- `engine/mod/divisions.h` — `kDivisions[17]` musical ladder (8 bars … 1/32, incl. dotted & triplet), `division_index(norm)`, `division_hz(idx, bpm)`. Already drives the modulator's SYNC mode.
- `engine/center/transport.h` — `Transport` with master BPM + external clock. BPM is always available (internal tempo even when global SYNC = Free).
- `engine/fx/flux.h` — `EchoDelay` topology already recirculates a band-limited, soft-clipped tape signal; feedback already unbounded-then-limited. The self-oscillation cap is a property of the in-loop limiter, so swapping the limiter is all that is needed for a musical bloom.
- Per-part base+modulation plumbing: `set_fx_target_base(slot, v)` + lane modulation, combined in `Part::fx_target_value(slot)`, applied in `PartFx::process`.

## Design

### 1. Control surface — two new knobs per part

FX knob row (y 88.9) fills its two free grid slots (aligned to `VOICE_X`):

| x | control | widget | function |
|------|--------------|--------|----------|
| 9.5  | `FLUX RATE`  | SMKNOB | synced division selector |
| 22.5 | `FLUX` (MIX) | SMKNOB | **unchanged** — send/mix |
| 74.5 | `FLUX FB`    | SMKNOB | feedback 0…>100 % |

New params: `FLUXRATE_A`, `FLUXRATE_B`, `FLUXFB_A`, `FLUXFB_B`.

**Patch compatibility (hard requirement):** append the four new params at the **end of `PARAMS`** in `gen_panel.py` (as FILT/TIDE/CHOKE already do), each with explicit coordinates — `RATE` at `x = 9.5 / W-9.5`, `FB` at `x = 74.5 / W-74.5`, both `y = 88.9`. Do **not** add them to the `part_controls()` template: that grows `PART_STRIDE` and shifts every part-B/SHARED param id, breaking existing `.vcv` patches.

### 2. Synced time (`FLUX RATE`) — synced-only

- The knob detents onto a **reduced division ladder**, a slice of `kDivisions` starting at **"1/2"**: the 12 rungs `1/2, 1/4., 1/2T, 1/4, 1/8., 1/4T, 1/8, 1/16., 1/8T, 1/16, 1/16T, 1/32` (indices 5..16 of `kDivisions`). Dotted and triplet included. Names come verbatim from `kDivisions[i].name` for the VCV tooltip.
- Delay time = `1 / division_hz(idx, bpm)`, always locked to the master TEMPO/clock. The old free-running 50 ms–5 s time is **removed**.
- **Buffer safety:** the longest rung (1/2 = 1 s @120 BPM, 2 s @60 BPM) fits the 5 s echo buffer down to ~24 BPM. A clamp to `(kMaxSamples / sr)` seconds stays as a safety net; it is effectively unreachable in normal use.
- **Slew:** shorten `EchoDelay` lag from 0.5 s to **~30 ms** — click-free on division changes, locks to the grid immediately. (The long tape slew existed for the now-removed TIME modulation wobble.)
- New helper in `divisions.h`: a FLUX-slice accessor (offset `= 5`, count `= 12`) that maps `norm → kDivisions` index without duplicating the table.

### 3. BPM plumbing

`Center`/`Instrument` forwards the current transport BPM to each part's `PartFx`, which forwards to `Flux`. `Flux` recomputes its delay time only when the **division index or BPM changes** (change-guarded), not per sample — negligible cost. New engine API: `Flux::set_rate(int slice_idx)` + `Flux::set_bpm(float)` (or a combined `set_synced_time(idx, bpm)`); `PartFx` gains a BPM input on `process()` or a `set_bpm()`.

### 4. Feedback >100 % with tanh bloom (`FLUX FB`)

- The **FB knob sets the base** of `FXT_FLUX_FB` (`set_fx_target_base(FXT_FLUX_FB, norm)`); the existing lane 4 modulation still rides on top for swells. This reuses the current base+mod path unchanged — only the knob source is new.
- Mapping: knob `0..1 → 0..1.1` (110 %) base; combined base+mod **clamped to ~1.2** (≈120 %). Displayed as a percentage in the tooltip. Exact top value is a play-test tuning detail.
- In `EchoDelay::Process`, **replace `SoftClip` with tanh saturation** in the recirculating path. At ~100 % it is near-transparent; above it the loop settles into a bounded, tape-warm self-oscillation — the same "bloom" model as the reverb's DECAY crossing 1.0. The amplitude bound is intrinsic to tanh, so **no separate envelope-follower/limiter is added**.

### 5. Modulation cleanup — remove TIME as an FX target

- Delete the `_flux.set_time(v[FXT_FLUX_TIME])` call in `PartFx::process`. FLUX time now comes **only** from the `RATE` knob path.
- **Lowest-ripple approach (approved):** keep the 5 lanes and the polyrhythm ratios / pad layout intact. Lane 1 keeps its engine (pitch/texture) role and simply loses its FX destination — the `_fx_depth[FXT_FLUX_TIME]` routing is retired. `FXT_FLUX_FB` is untouched; its base is now written by the FB knob.
- `FXT_FLUX_TIME` enum entry: keep the slot (so lane indexing/`FXT_COUNT = 5` is unchanged) but mark it FX-inert, or rename to reflect it is no longer routed. No compaction to 4 targets — that would disturb the lane↔pad mapping and the modulation identity.

### 6. CPU budget

Net ≈ ±0:
- Division→time recompute is change-guarded (a couple of ops on knob/tempo change).
- tanh replaces the existing `SoftClip` 1:1 (one transcendental per sample/channel, same as before — a polynomial approx is an option if profiling asks).
- No new buffers, no new envelope follower; the bloom is intrinsic to the loop topology.

## Testing

Extend `tests/test_flux.cpp`:
- **Synced time:** for a given BPM and division index, measured delay time == `1 / division_hz(idx, bpm)` within interpolation tolerance.
- **Buffer clamp:** at a very low BPM the longest division clamps to the buffer length rather than wrapping.
- **Bounded bloom:** feedback driven to ~120 % produces a *bounded* steady-state (no overflow, no runaway to full-scale/∞); at ≤100 % the tail decays.
- **Slew:** a division change reaches the new time within the shortened slew window without a discontinuity.

Adjust `tests/test_mod_tide.cpp` and `tests/test_part.cpp` where they exercise `FXT_FLUX_TIME` as a live modulation target (it is now FX-inert).

## Out of scope / deferred

- Rack play-test and listening pass (per project convention, deferred).
- Final numeric tuning of the feedback ceiling, tanh drive, and slew time — dialed in during the play-test.
- Repurposing the freed lane-1 FX destination for a new parameter — intentionally not done (YAGNI).

## Files touched (anticipated)

- `engine/fx/flux.h`, `engine/fx/flux.cpp` — synced-time API, BPM, tanh loop, feedback range.
- `engine/fx/part_fx.h`, `engine/fx/part_fx.cpp` — drop TIME-target application, BPM forward, FB-base wiring.
- `engine/mod/divisions.h` — FLUX division-slice accessor.
- `engine/center/*`, `engine/parts/part.*`, `engine/instrument.*` — forward BPM + `set_flux_rate`/`set_flux_fb` (base) down to the part.
- `host/vcv/res/gen_panel.py` + regenerated `generated_panel.hpp` — four appended params.
- `host/vcv/src/Spotymod.cpp` — configParam for RATE (division tooltip) & FB (percent tooltip); read knobs → `set_flux_rate` / `set_fx_target_base(FXT_FLUX_FB)`.
- `tests/test_flux.cpp`, `tests/test_mod_tide.cpp`, `tests/test_part.cpp`.
