# Spotykach dynamics — one-knob compressor per part + master limiter

**Date:** 2026-07-13
**Status:** approved design, pre-plan
**Depends on:** M4.5 ambient reverb v2 (landed), M1.6 FX chain, M6 shell spec
(for the two supersession notes below)

## Motivation

After the M4.5 reverb, full-wet experiments lack loudness: the send path
carries most of the audible signal and nothing lifts quiet material. The
fix is dynamics: a **one-knob compressor per part** (turning up = more
compression + auto-makeup, so quiet gets louder — that is the point) and a
**master limiter** at the very end of the chain as the safety ceiling.

Design intent, stated once and binding for the by-ear tuning: **the knob
is a loudness knob first**. When in doubt, tune the auto-makeup toward
"louder", not toward "cautious".

## Decisions (from the brainstorm)

| Question | Decision |
|---|---|
| Compressor position | end of `PartFx`, **before** the reverb send tap — dry AND send get compressed/gained; full-wet profits fully |
| Knob character | 0 = bit-exact bypass → glue → dense → **audible pumping in the top third** |
| Modulation | static parameter (`set_comp`, like `set_grit_mix`); the 5-lane FX-target row stays untouched |
| Limiter role | pure safety by default; global **master drive** (`pre_gain` 1–4×) available as a separate parameter |
| Compressor source | own module (MIT-clean). `daisysp::Compressor` lives in DaisySP-LGPL, which M4.5 deliberately dropped — not reversed |
| Limiter source | own ~25-line module after the stmlib recipe. The vendored `daisysp::Limiter` applies `SoftLimit(x * 0.7)` unconditionally (never bit-transparent, ~−3 dB coloring, mono per instance) — unusable as an invisible ceiling |

## Architecture

New modules in `engine/fx/`, Grit/Flux style (deterministic, no heap,
injected sample rate, unit-testable on desktop):

### `comp.h/.cpp` — class `Comp` (one per part, owned by `PartFx`)

Signal flow per part:

```
GRIT → FLUX → FX MIX → COMP → ┬→ dry out (l, r)
                              └→ send tap (equal-power) → shared reverb
```

- **Detector:** stereo-linked peak, `max(|L|,|R|)`, one-pole envelope;
  attack fixed ~5 ms, release mapped from the knob.
- **Gain computer:** threshold/ratio/soft-knee in dB. Runs **decimated
  every 16 samples** (the only log/exp code); the resulting gain is
  smoothed per-sample. This is what keeps the cost at ~0.3 % for both
  parts.
- **Bypass:** `amount` smoothed by a ~2 ms OnePole; below epsilon the
  process call is skipped entirely — bit-exact, same rule as "both FX
  blocks off costs nothing and changes nothing".

**One-knob mapping** (`amount` 0..1 → four internals; starting values,
final curve tuned by ear via host renders):

| amount | threshold | ratio | release | character |
|---|---|---|---|---|
| 0 | — | 1:1 | — | bypass (bit-exact) |
| ~0.33 | −10 dB | 2:1 | 80 ms | glue, soft knee |
| ~0.66 | −22 dB | 5:1 | 150 ms | dense, sustain-rich |
| 1.0 | −32 dB | 10:1 | 350 ms | deep pumping |

**Auto-makeup:** `makeup_dB = −threshold × (1 − 1/ratio) × 0.7`. The 0.7
compensation factor is the by-ear tuning handle; err toward loud (see
design intent). Quiet material below threshold is lifted by the full
makeup — the loudness win the feature exists for.

**[Amended by the by-ear pass, 2026-07-13]:** the makeup factor landed at
**0.9** (0.7/0.8 could not produce a rising loudness arc on the showcase),
which surfaced an audible failure mode: on dense full-wet material the
makeup lifted the program envelope to ~−4 dBFS and its peaks ground the
master limiter's ceiling continuously (heard as clipping). Three
implementation deltas fix this at the source, all in `comp.cpp`:
(1) **post-comp envelope ceiling** `kEnvCeiling = 0.4` (−8 dBFS) — the
gain computer never commands a gain that lifts the comp's own envelope
above the ceiling; quiet material sits far below the cap and keeps its
full makeup; (2) **asymmetric gain smoothing** — downward ~0.5 ms so
swells cannot outrun the ceiling, upward stays 2 ms; (3) **attack scales
with the knob** (5 ms at 0 → 2 ms at 1; supersedes "attack fixed ~5 ms")
so the envelope catches onsets before the cap can act. Sub-millisecond
transient tips still reach the master limiter — bending those is its
designed job.

### `limiter.h` — class `Limiter` (one, owned by `Instrument`)

Sits at the end of `Instrument::process`, after morph sum + reverb return:

```
morph(A,B) + reverb return → LIMITER → outL/outR
```

stmlib recipe (peak follower + `1/peak` gain riding), credited in
`THIRD_PARTY.md` like the Oliverb vendoring, with three corrections over
the DaisySP version:

1. **Stereo-linked**: one peak follower on `max(|L|,|R|)`, one gain for
   both channels.
2. **True bit-transparency**: gain riding only when `peak > 1`; the
   ceiling is a *piecewise* soft-clip that is exactly `out = in` below
   the knee (~−1 dBFS). This delivers the M6 shell spec's **Engine
   delta 3** (master soft-clip, bit-transparent below ~−1 dBFS) ahead of
   schedule — the shell spec gets a supersession note.
3. **Master drive**: `pre_gain = 1 + 3 × drive` (1–4×), default 0.

## Instrument API

```cpp
void set_comp(int p, float n);       // 0..1, boot default 0 (bypass)
void set_master_drive(float n);      // 0..1, boot default 0 (pure safety)
```

Pattern identical to `set_grit_mix` / `set_reverb_*`. No new FX-target
lane, no change to `FxMem` (no delay memory needed).

## Knob map (spec note for M6 — no implementation now)

- **COMP** per part: GRIT edit layer, **SMOOTH → COMP** (layer currently
  uses only RATE/DEPTH/SHAPE; SMOOTH, PROBABILITY, RANGE, TUNE are free).
- **MASTER DRIVE** (global): **FLUX-layer TUNE**, freed by the M4.5
  shimmer removal. Caveat: the reverb's DECAY/DEPTH axes also still seek
  their final M6 homes — collision resolution belongs in the M6 spec.

## Supersessions to record

1. M6 shell spec, "Engine deltas" #3 (master soft-clip): **delivered
   early by this milestone** as the limiter's transparent ceiling.
2. This spec adds the two knob-map suggestions above as input to M6.

## CPU & memory budget

- Compressors: ~15–20 cycles/sample per part (detector + smoothed gain;
  log/exp decimated ×16) → **~0.3–0.4 %** for both parts at 48 kHz on the
  480 MHz M7. Limiter: ~10–15 cycles on the stereo mix → **~0.15 %**.
  Total well under 1 % — an order of magnitude below the reverb (3–6 %),
  irrelevant against the < 70 % worst-case target.
- Memory: a handful of floats of state. No buffers, no `FxMem` change.
- Desktop render-time-per-block remains the early indicator (M4.5
  workflow); the hardware `METER` arrives with M6.

## Testing

Unit tests (doctest, `tests/`):

1. `amount = 0` → bit-exact passthrough (identical bits).
2. RMS increases monotonically across the amount sweep on a fixed test
   loop (auto-makeup works; the loudness intent, verified).
3. Attack/release time constants within tolerance (step response).
4. Stereo link: L-only signal → identical gain applied to R.
5. Limiter: peak ≤ 1.0 at 4× pre_gain; bit-transparent below the knee at
   `drive = 0`; stereo-linked like the compressor.
6. Determinism: two runs, identical bits.

By-ear (host renders): one new showcase scenario — a full-wet patch
riding COMP from glue into pump, plus a master-drive moment. Sound
character (does the pump breathe musically? does quiet material bloom
up?) is judged by ear, as with M4.5.

**Pinned renders:** `ambient_wash` was tuned to peak ≤ −0.8 dBFS, which
is *above* the −1 dB knee — its pinned WAV may change minimally once the
limiter lands. One-time re-pin, precedent exists (the M6 `fast_sin` swap
re-renders all pins anyway).

## Roadmap placement

New milestone **M4.6 — Dynamics** (after M4.5, before M5). Rationale:
like M4.5 it is a self-contained FX addition behind stable facades, and
every later milestone then renders demos through the loudness chain the
instrument will actually ship with. Also delivers M6 Engine delta 3
early, shrinking the M6 scope.

## Assumptions to verify during implementation

- The ×16 decimated gain computer introduces no audible zipper on fast
  pump settings (if it does: decimate ×8 or per-sample fast-approx).
- The −1 dB knee vs. the `ambient_wash` −0.8 dBFS peak: confirm the
  re-pinned render is audibly identical.
- The 0.7 makeup factor lands "loud enough" for the full-wet use case —
  the by-ear scenario exists to answer exactly this.
