# Spotykach — Roadmap & Status

Living status document for the modulation-first firmware fork. The README carries
the summary table; this file tracks the detail: what each milestone contains, what
is actually built today, and what is still design-only.

- **Design intent:** the residency design spec
  (`2026-07-10-spotykach-modulation-first-synth-design.md`), the scale spec
  (`2026-07-11-spotykach-scales-design.md`), the FX spec
  (`2026-07-11-spotykach-fx-design.md`), the center-section spec
  (`2026-07-12-spotykach-center-section-design.md`) and the ambient-reverb v2
  spec (`2026-07-12-spotykach-ambient-reverb-v2-design.md`).
- **Last updated:** 2026-07-13.

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
| **M3** | Capture sequencer (freeze the PITCH lane into a loop) | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **+ Entropy** | Looping S&H melody buffer; bipolar ENTROPY (erode / loop / grow) replaces EVOLVE | ✅ **done** (engine + host; switch mapping in M6) |
| **M4** | Center section — MORPH / COUPLE / DRIFT / SPOT / SETTLE | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M4.5** | Ambient reverb v2 — Oliverb port: Doppler SIZE, DECAY > 100 % bloom, TONE, DEPTH; shimmer + DaisySP-LGPL removed | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M4.6** | Dynamics — one-knob comp per part (glue → dense → pump, auto-makeup) + stereo-linked master limiter with MASTER DRIVE (delivers M6 engine delta 3 early) | ✅ **done** (engine + host; UI wiring deferred to M6) |
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
- **Modulation lanes** (`engine/mod/lane.*`) — two run modes, **FLOW**
  (smooth LFO) and **STEP** (clock-quantized sequences), plus the bipolar
  **ENTROPY** control (erode / loop / grow — see the entropy sequencer
  entry below). Own phase, own RNG stream, own probability dice per
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
firmware, a shared ambient reverb *(core replaced in M4.5 — Oliverb port,
shimmer removed)*, and 5 curated FX parameters per part as first-class modulation
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

### M3 — Capture sequencer ✅

Per-part freeze of the PITCH lane's last cycle into a replayable loop
(`capture_now` / `set_replay` in scenarios; `ALT + SEQ` on hardware, M6).
Capture swaps the lane's *source*, not the system.

- **CaptureLoop** (`engine/mod/capture.h`) — header-only double buffer
  (2 × 192 slots): the lane rolls its pre-smooth target + trigger pattern
  into the ring every generative sample; `capture_now` freezes the last
  full cycle. A dumb buffer — `ModLane` owns all slot timing, so record
  and replay share one phase→slot mapping.
- **ModLane replay** — recorded fired slots become the boundaries; live
  PROBABILITY dice thin the frozen loop (fail = hold), SMOOTH / RANGE /
  TUNE / quantizer stay live, ENTROPY is ignored on the replaying lane.
  Recording never touches the RNG — bit-determinism preserved.
- **SuperModulator / Instrument** — one loop per part, wired to the PITCH
  lane; `capture_now` / `set_replay` / `replaying` / `loop_valid`.
- **Host** — `capture_now`/`set_replay` scenario actions, `a_cap`/`b_cap`
  CSV columns, demos `capture_loop.json` + `capture_pentatonic.json` /
  `capture_duet.json`.
- **UI (M6)** — ALT+SEQ gesture, ring step-pattern display with playhead.

### Entropy sequencer ✅ (reworks the lane core, post-M3)

Listening to M3 renders showed STEP + S&H melodies were unusable note
salad (one random value per cycle, or pure noise per step). Now every
lane owns a looping 32-slot step buffer (seeded at init — a melody exists
from cycle one), and the LOOP/EVOLVE toggle became one bipolar **ENTROPY**
control:

- **0 — LOOP**: the melody repeats exactly (the LOOP contract, finally
  honored in the S&H zone).
- **> 0 — GROW**: fired steps mutate via a root-gravity random walk (small
  intervals common, leaps rare); the phase/shape/rate walk runs scaled by
  entropy.
- **< 0 — ERODE**: fired steps pull toward the root, note by note, down to
  a single tone; the walk settles back toward neutral.
- Mutation only on fired steps (suppressed steps hold note and slot);
  `shape_value()` returns the S&H operand exactly at SHAPE = 1; scenario
  action renamed `set_evolve` → `set_entropy`; demos
  `demo_step_melody.json` (entropy showcase) + `entropy_duet.json`.
- **UI (M6)** — panel switch 2 becomes ERODE / LOOP / GROW.

### M4 — Center section ✅

One `Center` class owns MORPH / COUPLE / DRIFT / SPOT, computed at control
rate (one 96-sample block) and wired through narrow `ModLane` /
`SuperModulator` / `Part` hooks — no engine-level branching.

- **MORPH** — fader, equal-power A↔B blend of both the dry mix and the
  reverb send (supersedes the M1.6 pre-morph-send rule: a fully
  morphed-away part injects no new reverb, only its already-committed tail
  rings out); boot default 0.5, smoothed.
- **COUPLE** — ALT + fader, a Kuramoto phase-locked loop between the two
  parts' master rates: mutual phase pull (no phase jumps) with a ×0.5..×2
  rate clamp, locking in 1–2 cycles at couple = 1 and staying quiet at low
  couple; a `sync_mode` anchor part holds its rate fixed as the other locks
  to it.
- **DRIFT** — SPOT-hold + fader, one shared Ornstein-Uhlenbeck "weather"
  walk (τ ≈ 45 s, bounded) feeding six hardcoded taps (rate ± ½ octave,
  shape ± 0.15, detune ± 25 cents, per lane); `set_drift` is smoothed.
- **SPOT** — per-lane random kick: a permanent ±½-cycle phase jump plus a
  ±0.35 shape jolt that decays back to 0 (τ ≈ 1.5 s); replay-immune (a
  captured loop ignores kicks).
- **SETTLE** — panic glide: DRIFT and the weather walk ease to 0, EVOLVE
  walk states re-center, and any open SPOT kick decays early; COUPLE and
  MORPH are untouched.
- Zero-effect invariant preserved: couple = 0 and drift = 0 reproduce every
  pre-existing lane/CSV column bit-for-bit, modulo the MORPH mix now being
  equal-power instead of the old unity-sum placeholder (a level-only
  change at the boot default).
- **Host** — five scenario actions (`set_morph`, `set_couple`, `set_drift`,
  `spot`, `settle`), five `mods.csv` global columns (`morph`, `couple`,
  `drift`, `weather`, `phase_err`), demos `couple_lock.json` (COUPLE
  convergence/anchor) + `weather_spot.json` (DRIFT weather + SPOT + SETTLE).
- **UI (M6)** — MORPH fader, ALT + fader for COUPLE, SPOT-hold + fader for
  DRIFT, SPOT tap gesture.

### M4.5 — Ambient reverb v2 (Oliverb port) ✅

The shared room becomes a playable instrument (spec:
`2026-07-12-spotykach-ambient-reverb-v2-design.md`, residency repo): vendored
MIT Oliverb core (Clouds Parasite) under `third_party/oliverb/` — float32,
48 kHz, deterministic. SIZE rescales the delay reads live (Doppler tail
warp), DECAY crosses 100 % at ~0.9 of its travel into a soft-limited bloom
(cap 1.05), TONE is the in-loop damping, DEPTH chorus-modulates the lines.
`set_shimmer` is gone (API + scenario action). Removing `ReverbSc` +
`PitchShifter` drops the DaisySP-LGPL dependency — the build is MIT-clean.
Facade, injection point (`FxMem`), and wet-only routing unchanged; the M6
shell places the ~130 KB object in SDRAM as before.

### M4.6 — Dynamics ✅

One-knob compressor per part (`engine/fx/comp.*`, end of the PartFx chain
BEFORE the reverb send tap — dry and send are compressed and auto-gained
together, so full-wet patches profit fully) plus a stereo-linked master
limiter (`engine/fx/limiter.h`, stmlib gain-riding recipe, exact
bit-transparency below the −1 dBFS knee) at the Instrument mix stage with
MASTER DRIVE (pre-gain 1–4×). The comp knob is a loudness knob first:
threshold/ratio/release/auto-makeup ride one macro (glue ~2:1 at a third,
dense ~5:1 at two thirds, 10:1 + 350 ms pumping at the top). API:
`set_comp(part, n)` / `set_master_drive(n)`, boot defaults 0/0. Delivers
the M6 shell spec's "Engine delta 3" (master soft-clip) early.

The by-ear pass reshaped the gain computer (spec amendment in the
residency repo): a **post-comp envelope ceiling** (−8 dBFS) stops the
auto-makeup from grinding program peaks into the master limiter,
downward gain moves act in ~0.5 ms, and the attack tightens with the
knob (5 ms → 2 ms) — quiet material still gets the full makeup, so the
loudness intent survives. Showcases: `comp_pump.json` (verification arc)
and `m7_bloom.json` (dev-diary render — one strummed Am7 into a long
room, the comp knob resurrects the dying tail). Spec + plan in the
residency repo (`2026-07-13-spotykach-dynamics-*.md`). M6 knob-map
suggestions: GRIT layer SMOOTH → COMP (per side), FLUX-layer TUNE
(ex-shimmer) → MASTER DRIVE.

## Planned

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
