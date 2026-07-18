# WAVE — PPG-style wavetable part engine

**Date:** 2026-07-18
**Status:** design approved, implementation gated on the hardware bench (see §7)
**Scope:** a third selectable part engine (`ENGINE_WAVE`) — a 4-voice wavetable
scanner behind the existing `IPartEngine`/`SynthEngine` semantics. One baked-in
curated bank, no new panel control, no new parameter id. No change to the mod
plane, FX, reverb, or the master chain.

## Problem

The SYNTH engine covers the analog-ish corner: polyblep morph sine→tri→saw→pulse
through an SVF. What's missing is the digital-glassy corner — PPG/Microwave
territory: bells, vocal formants, hollow resonant spectra, spectral junk — where
TIMBRE scans *through a bank of waveforms* instead of morphing between four
analog shapes. The SuperModulator lanes are the position modulator this class of
synthesis always wanted; the engine only has to hold still and be cheap.

CPU is the hard constraint: every published Daisy CPU figure proved unreliable
(bench-firmware spec, 2026-07-18), so this engine must be *structurally* cheaper
than SYNTH, and implementation waits for the on-hardware bench.

## Decisions (user)

- Character: **digital-glassy (PPG/Microwave)** — frame scanner, not additive,
  not phase distortion.
- Tables: **one curated bank, baked into the firmware** by a Python tool
  (`gen_panel.py` precedent). No SD, no runtime loading.
- Polyphony: **4 voices, full parity** with SYNTH (round-robin, steal, FLOW
  drone, chord layer, CHOKE hold).
- FILTER lane: **per-voice `daisysp::Svf` exactly like SYNTH** (RESO/FILT knobs
  keep their meaning).
- Voice interior: **2 detuned table reads + sub sine** — DETUNE knob works as
  today; only the oscillator core is swapped.
- DSP core: **hybrid scanner** — continuous frame lerp (audible, must be
  smooth), nearest mip with a control-rate crossfade (inaudible, may be cheap),
  int16 tables.

## Existing infrastructure this reuses

- `engine/synth/synth_engine.*`, `engine/synth/voice.*` — the entire allocation
  machine: round-robin + oldest-steal, FLOW sustain/demote, drone promise,
  chord slots + stab humanization, CHOKE hold, velocity slew, control-rate
  cadence (`kCtrlInterval = 96`).
- `engine/synth/morph_osc.h` — the oscillator interface WAVE's core mimics:
  `init / set_freq / set_detune_cents / set_morph / reset_phase / process`.
- `engine/synth/env.h`, `daisysp::Svf`, `engine/util/fast_sin.h` (sub sine),
  `engine/mod/rng.h` (drift rates) — unchanged, one level above the osc.
- `host/render/scenario.cpp` engine parsing; VCV per-part engine button
  (`Spotymod.cpp` ENGINE_A/B); `docs` bench-firmware workload list.

## Design

### 1. Integration — swap the oscillator, keep the engine

`WtOsc` implements the exact `MorphOsc` interface; `set_morph(0..1)` means
*bank position* instead of shape morph — same range, same control-rate feed,
same TIMBRE mapping (position + t² · DETUNE_MAX detune spread).

- `Voice` becomes `VoiceT<OscT>` and `SynthEngine` becomes
  `SynthEngineT<OscT>` — pure type parameterization, no behavior change.
  `using Voice = VoiceT<MorphOsc>` / `using SynthEngine =
  SynthEngineT<MorphOsc>` keep every existing name and test working; the SYNTH
  reference renders must stay **byte-identical** after the refactor.
- The new engine is `SynthEngineT<WtOsc>` (`using WaveEngine = ...`). All
  allocation/FLOW/chord/CHOKE semantics are inherited, not copied.
- `EngineId` grows `ENGINE_WAVE = 2`. `Part` holds the third engine instance
  (tables are `static const`, shared; the instance itself is small — voices +
  bookkeeping). Scenario parser learns `"wave"`. The VCV engine button cycles
  test-tone → synth → wave (LED shade distinguishes; panel unchanged —
  hardware-reducibility constraint). No other surface.
- VOICE edit layer (ATTACK/DECAY/RESO/SUB/DETUNE/FILT) applies literally —
  the setters live in `SynthEngineT` and are engine-agnostic already.

New files: `engine/synth/wt_osc.h` (header-only core),
`engine/synth/wt_bank.h/.cpp` (generated int16 data + metadata),
`tools/bake_wavetables.py` (generator; output committed, build needs no
Python).

### 2. Bank layout

- **16 frames.** Base mip 1024 samples per frame; halved-length mip chain
  1024 → 512 → 256 → 128 → 64 → 32 → 16 (7 levels, one per octave of
  fundamental) ≈ 2032 samples/frame ≈ 4 KB int16 → **bank ≈ 64 KB**, `static
  const` (QSPI flash). SDRAM untouched (FLUX tapes keep it).
- **Band-limiting happens at bake time.** Each mip level is spectrally
  truncated in the tool: a level of length N keeps partials up to ~N/4 — a
  guard margin for the images of linear interpolation. The exact guard factor
  is a bake constant, verified by the aliasing test (§8), not a runtime cost.
- int16 → float scaling is folded into the output gain (free).

### 3. WtOsc audio path

Per `process()` call, steady state:

1. Phase increment + wrap (float phase, as in `MorphOsc`).
2. Position ramp: one add per sample toward the control-rate target position
   (see §4).
3. Read: 2-tap linear interp in frame ⌊pos⌋ and frame ⌊pos⌋+1 at the current
   mip level; lerp by frac(pos) → **4 taps + 3 lerps**.

Mip selection runs at control rate: `level = clamp(floor(log2(f / f_base)))`.
On a level change the read crossfades old→new level over one 96-sample control
block (equal gain — the tables are near-identical below the truncation point);
only during that block does step 3 double. Pitch is latched at trigger in STEP,
so crossfades are rare; FLOW glides crossfade as they cross octaves.

No polyblep, no branches beyond the wrap and the crossfade flag, no libm in
the audio path. Structurally cheaper than `MorphOsc` (fast_sin polynomial +
branchy tri/saw/pulse + polyblep residuals).

### 4. Two custom details

- **Position ramp.** `set_morph` arrives once per 96 samples. Shape-morphing
  hid that; a frame scan can zipper. The position therefore glides linearly
  per sample to the new target across the control block (cost: 1 add). This is
  the only per-sample smoothing in the core.
- **No position wrap.** The bank is dramaturgically a *line* (dark → bright →
  digital); position clamps at both ends. TIMBRE extremes never jump across
  the bank seam.

Phase free-runs (no reset at trigger) — parity with `MorphOsc`. Sub sine,
detune split (± max/2 across reads A/B), drift LFOs, pan fan: all unchanged in
`VoiceT`.

### 5. Bank content — sweep dramaturgy

All frames are baked from harmonic recipes (per-partial amplitude + phase in
the Python tool — synthetic, deterministic, re-bakeable in seconds). Order is a
musical journey, because TIMBRE lanes will traverse it constantly:

| position | frames | character |
|---|---|---|
| 0.00–0.20 | 0–3 | sine → growing bell partials (odd/even mixtures) |
| 0.20–0.45 | 4–7 | vocal formants (ah → oh → eh) — the PPG sweet spot |
| 0.45–0.70 | 8–11 | hollow/resonant spectra (comb, fifth-heavy) |
| 0.70–1.00 | 12–15 | bright & digital: saw-adjacent → spectral junk, bit-organ |

The concrete recipes are **tuning material** — finalized in the deferred
listening test; the boundaries above are the intent, not a contract (same
philosophy as the DUST zone breakpoints).

### 6. Memory & data path

- Bank lives as `const int16_t` in the firmware image (QSPI-mapped flash).
- **Bench question:** random int16 reads from QSPI may be slower than internal
  RAM. If the bench shows it, the fallback is copying the 64 KB bank into
  SDRAM at boot (64 KB of 64 MB; the FLUX tapes don't care). The core reads
  through a base pointer either way, so the fallback is a boot-time memcpy,
  not a redesign.
- Desktop/VCV hosts compile the same generated `wt_bank.cpp` — identical bits
  on every platform.

### 7. CPU budget & the hardware gate

Expectation: WAVE 2×4 voices ≤ SYNTH 2×4 (bench spec estimate 15–18 %),
because the per-sample core is strictly simpler. That stays an *estimate*
until measured:

- The bench firmware's workload list grows one entry: **“WaveEngine 2×4
  voices (both parts)”** — same harness, same anchor calibration.
- **Implementation of this spec starts only after the hardware bench has run**
  and confirms (a) the WAVE workload fits alongside the full instrument and
  (b) the QSPI-vs-SDRAM answer (§6).

### 8. Testing

- **Parity:** the existing SynthEngine semantic tests (FLOW drone, chords,
  steal order, CHOKE) run as a second template instantiation against
  `SynthEngineT<WtOsc>`. SYNTH reference renders stay byte-identical after the
  templatization (regression gate for the refactor itself).
- **Aliasing:** a pitch-chirp render across the full register, FFT-checked:
  no partial above the Nyquist guard line, threshold in dB as a test constant.
- **Continuity:** position-sweep render has no derivative jumps; mip
  crossfades are click-free.
- **Determinism:** same seed → bit-identical render on desktop and VCV (int16
  tables are exact; no float bake drift).
- **Reference scenario:** a new `wave_formant_sweep.json` (TIMBRE lane
  traversing the formant zone in FLOW) as listening + regression anchor.

## Out of scope

- SD/user wavetables, multiple banks, a bank-select control.
- Position wrap modes, internal position envelopes (the mod plane *is* the
  position modulator).
- Unison beyond the existing 2-read detune; mip-tilt filter coupling
  (considered, rejected in favor of plain SVF parity).
- Any panel/param change beyond the engine cycle that already exists.
