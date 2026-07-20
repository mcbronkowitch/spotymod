# Spotykach Reverb Dry/Wet Mix — Design

Adds a single MIX control to the shared ambient reverb: equal-power dry/wet
crossfade at the master sum, with a true CPU-saving bypass at 0 %. The reverb
stays a send/return — dry never passes through the reverb core; the knob is
two gains at the mix point plus a sleep gate.

## Goal

Today the reverb balance is fixed: dry at unity, wet trimmed by the internal
`kWetGain` (0.40, −8 dB headroom for the bloom). There is no way to go
wet-only for ambient washes, and no way to switch the room off — the Oliverb
core (the most expensive block in the engine) runs even when unused. One
knob fixes both: 0 % = reverb genuinely off (no `process()` call), 50 % =
equal balance, 100 % = wet only.

## Decisions (from brainstorming, 2026-07-14)

- **Equal-power crossfade** — `dry = cos(m·π/2)`, `wet = sin(m·π/2)`.
  Constant perceived loudness across the travel; at 50 % both gains sit at
  ~0.707 (a 1:1 ratio, satisfying "50/50").
- **`kWetGain` stays** as an internal calibration inside the reverb; the mix
  multiplies on top. 100 % wet therefore equals today's wet level — slightly
  quieter than dry, but the bloom keeps its guaranteed headroom and the
  master limiter is not leaned on. (Its stale "until the M6 master
  soft-clip exists" comment gets corrected — the limiter shipped in M4.6.)
- **Clear-on-sleep bypass** — when the knob reaches 0, the smoothed wet gain
  fades out, the delay buffer is zeroed once, and the reverb sleeps
  (`process()` skipped, sends discarded). Waking starts from a clean, empty
  room — no ghost tail from minutes ago, and a self-oscillating bloom is
  genuinely killed at 0 %.
- **Send input is untouched by MIX** — the tail character does not change
  while turning the knob; only the output balance moves.

## Signal flow

```
Part A/B post-FX × REVERB SEND (equal-power, morph-scaled per M4)
        → AmbientReverb (shared, wet-only, kWetGain inside)
part mix × dry_gain  +  reverb out × wet_gain  → limiter → out
```

Same injection point and send semantics as M4.5; only the join gains change.
The mix point lives in `Instrument::process` — the only place where dry
(the part mix) and wet (the reverb return) exist as separate signals.

## Architecture

### Engine — `engine/instrument.h/.cpp`

- New setter `set_reverb_mix(float m)` (0..1, clamped) beside the four
  existing reverb setters. It computes the two gain **targets**
  (`cos`/`sin` once per call — control-rate libm, same policy as
  `set_tone`) and stores them; no per-sample trig.
- Two `OnePole` smoothers (`util/onepole.h`) on the dry and wet gains,
  ~10 ms (ear-tunable), processed per sample in `Instrument::process`.
  Equal-power holds to within the smoother's short settling — no zipper,
  no audible dip.
- Sleep gate, all inside the `if (_reverb)` branch:
  - **Falling asleep:** when the wet target is 0 and the smoothed wet gain
    has snapped to 0 (the `OnePole` snaps exactly), call the new
    `AmbientReverb::clear()` once and set `_reverb_asleep = true`. While
    asleep, `_reverb->process()` is skipped and the sends are discarded;
    dry gain is exactly 1.0 (cos 0), so the output is bit-identical to the
    part mix.
  - **Waking:** any `set_reverb_mix` target > 0 clears `_reverb_asleep`;
    processing resumes immediately into the cleaned buffer while the wet
    gain fades up from 0. No click, no ghost tail.
- Null-reverb path (`init` without `FxMem`) behaves as today: pure part
  mix, mix setter is a stored no-op.

### Facade — `engine/fx/reverb.h/.cpp`

- New method `clear()`: zeroes the delay buffer and the loop/LFO filter
  state, **leaving the parameter state intact** (unlike a re-`Init`, which
  would reset diffusion/size/decay and the RNG seed). Implementation
  detail: memset of `_buffer` plus whatever per-line state Oliverb exposes;
  if the vendored core needs a small `Clear()` addition, that is an
  allowed modification (documented in `THIRD_PARTY.md` as before).
- `kWetGain` untouched; its stale comment corrected.

### Defaults & calibration

- **Default mix = 0.25, chosen by ear (2026-07-14).** Because MIX
  multiplies on top of the internal wet trim, the old fixed balance
  (dry 1.0 / wet-total 0.40) maps to MIX 0.5 — at −3 dB overall (both
  gains 0.7071). *(Correction: this spec originally claimed 0.25
  reproduces the old ratio; that conflated the knob gain with the total
  wet gain.)* At the chosen 0.25 the dry level is kept (−0.7 dB) and the
  room sits −8.3 dB leaner than the old join; measured on `ambient_wash`:
  −2.2 dB RMS vs. the pre-M4.8 baseline. Auditioned against 0.35 / 0.40 /
  0.5 renders and picked deliberately.
- REVERB SEND default stays 0.25 (M1.6). SEND shapes how much of each part
  feeds the room; MIX shapes how much room reaches the master.

## Migration

| Touchpoint | Change |
|---|---|
| `engine/instrument.h/.cpp` | `set_reverb_mix` added; gain smoothers + sleep gate in `process` |
| `engine/fx/reverb.h/.cpp` | `clear()` added; `kWetGain` comment corrected |
| `host/vcv/res/gen_panel.py` | `REV_MIX` added to the shared center strip; `generated_panel.hpp` + panel SVG regenerated |
| `host/vcv/src/Spotymod.cpp` | fifth reverb param `REV_MIX` (default 0.25), forwarded like the others |
| VCV plugin binary | recompiled + reinstalled with the documented build env (Rack-SDK 2.6.6, WinLibs mingw, MSYS2 make, `EXTRA_CXXFLAGS=-std=c++17`) so the new knob is usable in Rack |
| `host/render/scenario.cpp` | action `set_reverb_mix` added |
| Scenario JSONs | none required — absent mix means default 0.25 (≈ today's balance). `ambient_wash` optionally gains a MIX ride later; out of scope here |
| `tests/test_instrument.cpp` | new mix/bypass tests (below) |
| `docs/roadmap.md` | new milestone entry **M4.8** |

### UX note (M6, deferred)

The reverb layer gesture (`ALT + FLUX` hold) now has five axes for four
knobs. Candidate: MIX takes the RANGE knob position in the reverb layer
(unused there so far); tuned on hardware in M6. The engine API is agnostic.

## Testing

doctest, desktop, deterministic — as established:

1. **Mix 0 is bit-exact and asleep:** set mix 0, run past the smoother
   settling, then feed input — output is bit-identical to the engine-only
   part mix (compare against an `Instrument` initialized without reverb).
2. **Mix 1 kills dry:** with a dry-only signal and sends muted
   (REVERB SEND 0), mix 1 → output is silence (post-settling).
3. **Equal-power midpoint:** at mix 0.5 both effective gains are ~0.7071
   (ratio 1:1 within float tolerance), verified via amplitude measurement
   on a dry-only and a wet-only path.
4. **No ghost tail after sleep:** build a loud tail, turn mix to 0, run
   until asleep, turn mix up with silent input → output stays silent
   (buffer was cleared).
5. **No zipper:** step mix 0→1 mid-note; sample-to-sample output delta
   stays bounded (smoother works).
6. **Determinism:** identical scenario with mix automation → bit-identical
   output across two runs.
7. Existing suites stay green; null-reverb pointer safety adapted, not
   weakened.

By-ear scope: default-mix render of `ambient_wash` against the current
build (default choice is a taste decision, see Defaults & calibration),
plus one wet-only listen.

## CPU

At mix 0 the Oliverb core is skipped entirely — the reverb's ~3–6 % M7
budget drops to zero (minus two smoothers and a branch). The one-time
buffer memset (~130 KB) on falling asleep costs a few µs once, at a moment
when the wet path is already silent.

## Roadmap placement

**M4.8 — Reverb dry/wet mix** (after M4.7 melody engine rework, before M5).
Self-contained: touches only the master join and the reverb facade, no
interaction with the M4.7 or M5 scope.

## Acceptance criteria

- `set_reverb_mix` exists engine-side, in the VCV host (REV_MIX) and the
  render host (`set_reverb_mix` action); default 0.25 everywhere.
- The VCV plugin builds clean with the new knob on the panel and is
  reinstalled into Rack (manual smoke test: MIX at 0/50/100 % behaves as
  specified).
- All new tests pass; existing suites stay green.
- `ambient_wash` render at defaults signed off by ear against the M4.7
  baseline (default is deliberately leaner, see Defaults & calibration);
  a wet-only render confirms dry is fully gone at 100 %.
- Grep-clean: the stale `kWetGain` comment about the missing master
  soft-clip is gone.
