# Spotykach — Roadmap & Status

Living status document for the modulation-first firmware fork. The README carries
the summary table; this file tracks the detail: what each milestone contains, what
is actually built today, and what is still design-only.

- **Design intent:** the residency design spec
  (`2026-07-10-spotykach-modulation-first-synth-design.md`), the scale spec
  (`2026-07-11-spotykach-scales-design.md`) and the FX spec
  (`2026-07-11-spotykach-fx-design.md`).
- **Last updated:** 2026-07-11.

> **Reminder:** nothing here has run on real hardware yet. Everything below is
> verified only against the desktop offline renderer (unit tests + WAV/CSV
> render). The Daisy firmware shell is milestone **M6**.

## Status at a glance

| Milestone | Scope | Status |
|-----------|-------|--------|
| **M1** | Portable engine foundation: SuperModulator, five lanes, `Instrument` API, desktop render host + tests | ✅ **done** |
| **+ Scales** | Pitch quantization (6 scales, SCALE/CHROM/FREE, root) layered onto the PITCH lane | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M1.6** | FX: per-part FLUX (tape echo) + GRIT (drive/reduce), shared ambient reverb, FX params as modulation targets | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M2** | Polyphonic synth voice (replaces the M1 test tone) | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M3** | Capture sequencer (freeze the PITCH lane into a loop) | ⬜ planned |
| **M4** | Center section — MORPH / COUPLE / DRIFT / SPOT | ⬜ planned |
| **M5** | Sampler engine adapter (granular Deck/Vox) | ⬜ planned |
| **M6** | Firmware shell: pads, gestures, panel, LEDs — runs on real hardware | ⬜ planned |

Milestone order follows the design spec's build order (audible first, hardware
last). The scale layer was inserted after M1 because it only touches the PITCH
lane's output stage and needed no new engine. M1.6 sits before M2 so that
M2–M5 build on the final signal chain (part FX + reverb sends) from the start
instead of rewiring it later; the M1 test tone is enough to hear and verify
the effects in the renderer.

## Done

### M1 — Portable engine foundation ✅

The complete modulation core, host-independent (`engine/` has no libDaisy
include), audible via the desktop renderer.

- **SuperModulator per part** (`engine/mod/super_modulator.*`) — one macro
  surface (RATE, SHAPE, PROBABILITY, SMOOTH, RANGE, DEPTH) over **five
  independent lanes**, each at a fixed musical ratio of the master rate.
- **Modulation lanes** (`engine/mod/lane.*`) — three run modes:
  **FLOW** (smooth LFO), **STEP** (clock-quantized sample & hold), **ENTROPY**
  (per-cycle random walk). Own phase, own RNG stream, own probability dice per
  lane. Continuous waveform morph (sine → triangle → ramp → pulse → S&H) in
  `engine/mod/waveforms.h`; RANGE mapping (off → unipolar → bipolar) in
  `engine/mod/range.h`; deterministic RNG in `engine/mod/rng.h`.
- **Part + engine interface** (`engine/parts/*`) — `Part` routes lane outputs to
  targets and exposes `pitch_cv()` / gate. `engine_iface.h` is the sound-engine
  boundary; `test_tone_engine.h` is the M1 placeholder engine (110–880 Hz over
  the 0..1 pitch contract).
- **Public API** (`engine/instrument.h`) — `init(sample_rate)`, normalized
  `0..1` setters, `process(in, out, size)`. Single boundary for both hosts.
- **Desktop render host** (`host/render/`) — scenario JSON → 16-bit stereo WAV +
  `mods.csv` (every lane, pitch CV, gate). Vendored `nlohmann/json`.
- **Tests** (`tests/`, doctest) — lane STEP quantization, ENTROPY loop/grow/erode
  determinism, per-step dice, RANGE mapping, RNG determinism, SuperModulator,
  Part routing, WAV writer, scenario parsing.

### Scale quantization ✅ (extends M1)

Melodies sit in a musical scale by default, with chromatic and free modes for
drift experiments. Engine + host complete; the hardware gestures are specified
but not yet wired (that is UI work, i.e. M6).

- **Quantizer module** (`engine/pitch/quantizer.h`) — near-stateless
  `SCALE / CHROM / FREE`, 12-bit scale mask, root in semitones, ±15-cent
  hysteresis at raster boundaries, ~30–50 ms change slew (FREE is a bit-exact
  passthrough).
- **6 scales, dark → bright:** minor pentatonic, Aeolian, **Dorian (default)**,
  major pentatonic, Lydian, whole tone. Boot default: Dorian, both parts SCALE.
- **Placement** — last stage of `Part::target_value(LANE_PITCH)`, so SMOOTH
  glides step through scale notes and ENTROPY grows or erodes the melody. `pitch_cv()` is
  the single quantized source of truth for engine, CV out, and the future
  capture sequencer.
- **Host** — `set_scale`, `set_quant_mode`, `set_root` scenario actions; demo
  scenarios `dorian_melody.json`, `pentatonic_melody.json`, `dorian_vs_drift.json`.
- **Tests** (`tests/test_quantizer.cpp` + Part integration) — scale mapping,
  hysteresis, change-slew settle time, FREE passthrough, root shift, and that
  `pitch_cv()` only lands on allowed scale degrees in SCALE mode.

### M1.6 — FX ✅

Per-part FLUX (tape echo) + GRIT (drive/reduce) ported from the original
firmware, a shared ambient reverb (DaisySP `ReverbSc` + optional +12 st
shimmer), and 5 curated FX parameters per part as first-class modulation
targets — a second tap on the same five lanes (fixed 1:1 lane → target
mapping, `engine/fx/part_fx.h`).

- **`engine/fx/`** — `fx_util.h` (XFade/SoftSwitch ports), `grit.*`, `flux.*`,
  `reverb.*`, `part_fx.*`. DaisySP is now an `engine/` dependency (portable
  C++; still no libDaisy). Memory is injected (`FxMem`): echo buffers +
  reverb object — static on desktop, SDRAM on Daisy (M6).
- **Signal flow** — per part: engine → GRIT → FLUX → FX MIX (equal-gain
  dry/wet); post-FX send × REVERB SEND (equal-power) into the shared room,
  which joins the master after the part mix. Bypass is bit-exact.
- **Host** — 10 new scenario actions, 5 FX columns per part in `mods.csv`,
  demo scenarios `dub_delay.json` / `ambient_wash.json`.
- **UI (M6)** — FLUX/GRIT pads, hold-layers, ALT gestures per the FX spec.

### M2 — Polyphonic synth voice ✅

4-voice trigger-driven synth engine (`engine/synth/`) is the boot-default
part engine; `TestToneEngine` stays selectable (`set_engine` — tests, A/B
reference).

- **Voice** — 2× polyblep `MorphOsc` (single phasor, continuous
  sine→tri→saw→pulse, detune in cents) + sub sine → DaisySP `Svf` lowpass →
  exponential AD/ADS envelope (retrigger-from-level) → equal-power pan with
  slow deterministic per-voice drift. Audio-path sine is the shared
  polynomial `fast_sin` (`engine/util/fast_sin.h`) — no libm `sinf` in the
  voice path; drift + envelope coefficients update at control rate
  (96-sample blocks). CPU-budget constraints from the spec.
- **Engine** — round-robin allocation, oldest-steal with retrigger-from-
  level; STEP = plain AD notes; FLOW = sustaining-last-voice drone (sustain
  0.7, pitch continuously follows the quantized PITCH target; entering FLOW
  with no sustaining voice auto-triggers — the drone promise). Targets:
  TIMBRE (morph + t²·DETUNE_MAX detune), FILTER (60 Hz–14 kHz exp), PITCH
  (latched at trigger, 110·8^p), MOTION (pan fan ±1/±0.5 × width + drift),
  LEVEL (smoothed master gain).
- **Tempo-coupled envelopes** — attack/decay are ratios of the master
  modulation cycle (defaults 2 % / 1.5×, attack floor 2 ms, decay clamp
  50 ms–20 s), edited via `set_voice_attack/decay/resonance/sub/detune`
  (VOICE layer; hardware gestures in M6).
- **Part / Instrument** — `set_engine(EngineId)` with a click-free
  SoftSwitch crossfade; `set_cycle`/`set_flow` forwarding (default no-ops on
  `IPartEngine`); `trigger_manual` (PLAY tap); `active_voices` / `voice_env`
  introspection.
- **Host** — 7 new scenario actions; `voices` + `v0..v3` CSV columns; demo
  scenarios `overlapping_voices.json` (the master spec's M2 acceptance demo)
  and `flow_drone.json`. Existing scenarios pinned to `ENGINE_TEST_TONE`.
- **UI (M6)** — VOICE edit layer gestures (PLAY-pad hold), PLAY-tap manual
  trigger wiring, engine-switch gesture.

## Planned

### M3 — Capture sequencer ⬜
Per-part freeze of the PITCH lane's last cycle (pitch steps + trigger pattern) —
swaps the lane's *source*, not the system. RATE still drives loop speed, PROBABILITY
thins live, SMOOTH glides, TUNE transposes, ENTROPY affects only live lanes.
Stores **raw** lane values so re-scaling re-voices the captured melody. Volatile.

### M4 — Center section ⬜
MORPH (fader, equal-power A↔B), COUPLE (ALT + fader, mutual phase/rate pull),
DRIFT (SPOT-hold + fader, slow global weather), SPOT tap (per-lane random
phase/shape jolt). Fader layering with slew catch-up; CV_CROSSFADE always acts on
MORPH.

### M5 — Sampler engine adapter ⬜
Adapter around the existing granular Deck/Vox engine (depends only on `Buffer`)
behind `engine_iface`. Targets: POSITION, SIZE, PITCH, SHAPE, LEVEL. Record-arm
transport; desktop host loads WAVs from disk instead of SD.

### M6 — Firmware shell ⬜
Thin Daisy shell hosting `engine/` next to the original `app.cpp` (kept
buildable). Wires up pads (release-based tap/hold gestures), the three 3-position
panel switches, LED ring / pad / CYCLE feedback, CV + gate + V/Oct + clock I/O,
preset persistence, and the deferred scale gestures (ALT-hold inspect, ALT+TUNE
scale select, ALT+PITCH-pad mode cycle). **First milestone that runs on real
hardware.**

## Build & verify

```bash
source env.sh            # optional: toolchain on PATH, CC/CXX
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/render.exe host/render/scenarios/dorian_vs_drift.json out.wav mods.csv
```

See the README for the full desktop and (upstream) hardware build instructions.
