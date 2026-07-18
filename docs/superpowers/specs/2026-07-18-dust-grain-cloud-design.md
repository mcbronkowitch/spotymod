# DUST — grain cloud + tape freeze on the FLUX echo

**Date:** 2026-07-18
**Status:** design approved, ready for implementation plan
**Scope:** a per-part granular read stage inside the FLUX block, plus a tape FREEZE. No new audio buffers, no change to GRIT, COMP, reverb, or the master chain.

## Problem

Clouds-style granular texture is wanted — smearing, freeze/stutter, random scatter — but a Clouds port would be redundant (its reverb/diffusion already exists as ROOM) and too heavy for the remaining Daisy budget. The insight that makes a native version nearly free: **the FLUX tape already holds 5 s of per-part stereo material**. A granulator can run as extra read heads on that existing tape — zero extra sample memory, and echo + grains share one sonic identity ("the tape"), closer to Beads' delay-granular hybrid than to a Clouds copy.

Explicitly out of desire (user decision): pitch-shifted grains. In: texture/smear, freeze/stutter, random scatter.

## Existing infrastructure this reuses

- `engine/fx/flux.h` — `EchoDelay` over `DeLine` ring buffers (240 000 samples = 5 s per channel per part). The write pointer decrements once per sample; a read at constant offset behind it **is** 1× forward playback delayed by that offset.
- `engine/mod/rng.h` — deterministic xorshift32 `Rng` (bit-reproducible, statistically testable).
- `engine/util/fast_sin.h` — for the raised-cosine (Hann) grain window.
- `engine/fx/part_fx.*`, `engine/instrument.*` — per-part FX plumbing and setter forwarding.
- `host/vcv/res/gen_panel.py` — panel generator; params appended at end of `PARAMS` for patch compatibility (FILT/TIDE/FLUXRATE precedent).

## Design

### 1. Core insight — grains without resampling

A grain never moves relative to the write head: it is a **constant integer offset** behind it. Because the write pointer advances every sample, a constant offset is exactly 1×-speed forward playback (same mechanism as the echo read head). A grain is therefore only:

- a random integer offset (fixed for its lifetime),
- an age counter + length (Hann envelope),
- fixed equal-power pan gains,
- a source channel choice (reads the L **or** R tape, random per grain — free stereo decorrelation).

Per grain per sample: 1 array read (no interpolation — offsets are integer) × window × 2 pan multiplies.

### 2. DustCloud block (per part, lives inside `Flux`)

New `engine/fx/dust.h/.cpp`: `DustCloud` with a fixed pool of **8 grains** (plain structs, no heap).

- **Scheduler:** countdown to next grain birth; interval drawn from a range set by the macro value, randomized ±50 %. If no slot is free the birth is dropped (natural density ceiling).
- **Envelope:** Hann (raised cosine) via `fast_sin` or a small constant LUT — implementation detail for the plan; requirement is click-free grain edges.
- **Output normalization:** grain sum scaled by `1/sqrt(expected overlap)` so perceived level stays roughly constant across the density range.
- **Determinism:** own `Rng`, seeded per part with fixed distinct seeds at init. Same input + same settings ⇒ bit-identical output (desktop == firmware, testable).
- **Tape access:** `DeLine`/`EchoDelay` gain a raw `ReadTap(int offset)` (no filter, no tanh). Minimum offset ~10 ms so a grain never chases the write seam.

### 3. The DUST macro knob — one curve, whole journey

One per-part knob `g ∈ 0..1` drives everything (Clouds' blend philosophy):

| g | character | density (mean birth interval) | spray (max offset) | grain length range |
|------|----------------------|------------------------------|--------------------|--------------------|
| 0 | off — path skipped, **bit-exact** with today | — | — | — |
| ~0.3 | sparse splinters | ~600 ms | ~150 ms behind head | 25–100 ms |
| ~0.7 | dense cloud joins | ~60 ms | ~2 s | 50–250 ms |
| 1.0 | cloud takes over | ~30 ms (full overlap) | full 5 s | 80–400 ms |

- Density and spray map **exponentially**; all endpoint numbers are tuning constants in one header block, finalized in the (deferred) play-test.
- **Head takeover:** below g ≈ 0.7 the echo read head is untouched; from 0.7 → 1.0 the echo head output fades to zero (equal-power), so at full DUST only the cloud remains — one knob morphs tape echo into granular cloud.

### 4. Signal flow (in `Flux::process`)

```
send = sw.process()
e   = echo.Process(in * send)            // unchanged: bpf → tanh → write(fb)
gLR = dust.process()                     // reads ReadTap() on both channel tapes
out += (e * head_gain(g) + gLR) * _mix_lin
```

- Grain sum is added **before** `_mix_lin`: the FLUX (MIX) knob remains the single wet control for everything coming off the tape.
- Grains do **not** pass the band-pass/tanh and do **not** enter the feedback write (approved approach 1): the cloud is rawer and brighter than the echo — its own character, zero runaway risk. (Generation-smearing still happens indirectly: echo feedback keeps re-writing repeats onto the tape the grains read.)
- `g == 0` skips the dust path entirely; FLUX off (SoftSwitch idle) skips the whole block — both bit-exact, both free.

### 5. FREEZE — the write head stops storing but keeps moving

`EchoDelay::SetFreeze(bool)`: when frozen, `Process` skips the store **but still advances the write pointer**. Consequences, all for free:

- Echo and grain taps travel through the frozen material on their own — the echo head becomes a **5 s looper** (its read position scans the whole frozen tape once per 5 s; the RATE setting only phases it), the grains scatter the still image.
- The grain code has **no freeze special-case at all**.
- The feedback loop is paused (nothing is written); input keeps passing dry around FLUX as always.
- Unfreeze simply resumes writing; the seam between old and new material is honest tape aesthetic. If the play-test finds it clicky, a short write-in crossfade is the polish, not part of v1.

FREEZE works independently of the DUST knob (echo-looper alone is a feature).

### 6. Control surface + modulation policy

Per part (mirrored A/B like the FLUX cluster):

- **`DUST_A/B`** — SMKNOB, the macro knob, in the FX box next to GRIT/COMP (`FX_BOT` gains a third slot; exact x chosen in `gen_panel.py` when laying it out).
- **`FRZ_A/B`** — LATCH pad (same widget family as ENG/GRIT pads).
- **`FRZGATE_A/B`** — gate input in the jack strip: latch state **OR** gate high = frozen, so a sequencer can stutter-freeze rhythmically.

**Patch compatibility (hard requirement):** all new params/inputs are appended at the **end** of their enums in `gen_panel.py` with explicit coordinates — never added to the `part_controls()` template (that would shift part-B/SHARED ids and break existing `.vcv` patches).

**No FX-target lane** (user decision): DUST is hand-played; rhythmic freezing comes from the gate input, not the internal modulation. The 5-lane == pad-slot structure stays untouched.

**Hardware reducibility note:** worst case on the real panel this is +2 small knobs, +2 pads, +2 jacks. Reduction path if space runs out: one shared FRZ button (pressing both parts) and/or dropping the gate jacks — the engine API (`set_dust(p, g)`, `set_freeze(p, on)`) is per-part regardless, so the panel can merge without engine changes.

### 7. Engine API + host plumbing

- `Flux::set_dust(float norm)`, `Flux::set_freeze(bool)`; `Flux` owns the `DustCloud` and passes it tap access.
- `PartFx::set_dust/set_freeze` → `Instrument::set_dust(p, v)/set_freeze(p, on)` (same forwarding pattern as `set_flux_mix`).
- `host/render/scenario.cpp`: new actions `set_dust`, `set_freeze` (+ a demo scenario, e.g. `dust_cloud.json`).
- `host/vcv/src/Spotymod.cpp`: read knob/pad/gate → setters; DUST tooltip in percent, FRZ as on/off.

### 8. CPU + memory budget

- **Memory:** ~8 grain structs × 2 parts (< 1 KB). **No new audio buffers** — the entire 5 s × 4-channel cost is already paid by FLUX.
- **CPU:** worst case 16 active grains total × (1 read + window + 2 mults) per sample, plus a countdown decrement. No transcendentals in the hot path (window via LUT/fast_sin). Estimated well under 1 % on the H750 @ 480 MHz; negligible next to the synth voices.
- When DUST is 0 and FREEZE off, added cost is two branch checks.

## Testing

New `tests/test_dust.cpp` (+ small additions to `tests/test_flux.cpp`):

- **Bypass bit-exactness:** `g = 0`, freeze off ⇒ output identical to pre-DUST FLUX, sample for sample.
- **Determinism:** two runs, same seed/settings/input ⇒ bit-identical output.
- **Freeze stops the tape:** with freeze on, buffer contents are unchanged after N samples of loud input, while output (loop + grains) is non-silent; echo output repeats with a 5 s period.
- **Freeze passthrough:** dry input still reaches the output while frozen.
- **Click-free grains:** max sample-to-sample delta of the grain sum stays bounded across many births/deaths (envelope integrity).
- **Level bound:** full-scale input, g = 1 ⇒ grain sum stays within headroom (normalization works).
- **Scheduler statistics:** measured birth rate for a given g within tolerance of the mapped interval (style of `test_rng.cpp`).

## Out of scope / deferred

- Rack play-test and listening pass (project convention) — final tuning of all macro-curve endpoints, takeover knee, and normalization there.
- Unfreeze write-in crossfade — only if the play-test hears a click.
- Pitch-shifted grains, grain feedback into the tape (approach 2), reverse grains — deliberately not in v1. Approach 2 remains a one-line write-path extension if self-regeneration is ever missed.
- Firmware (M6) wiring — engine code is target-agnostic like the rest of the FX layer.

## Files touched (anticipated)

- `engine/fx/dust.h`, `engine/fx/dust.cpp` — **new**: `DustCloud` (pool, scheduler, window, macro curve).
- `engine/fx/flux.h`, `engine/fx/flux.cpp` — `ReadTap`, `SetFreeze`, dust integration in `process`, head-takeover gain.
- `engine/fx/part_fx.h/.cpp`, `engine/instrument.h/.cpp` — setter forwarding.
- `host/vcv/res/gen_panel.py` + regenerated `generated_panel.hpp` — `DUST_A/B`, `FRZ_A/B`, `FRZGATE_A/B` appended.
- `host/vcv/src/Spotymod.cpp` — param config, tooltips, gate OR latch logic.
- `host/render/scenario.cpp` + `host/render/scenarios/dust_cloud.json` — scenario actions + demo.
- `tests/test_dust.cpp` (new), `tests/test_flux.cpp`.
