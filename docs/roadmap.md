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
- **Last updated:** 2026-07-19 (fast tanh in the echo loop and the master limiter, measured at `87f3538`; worst case 104 % -> 96 % anchored max — **under budget for the first time, gate and all**).

> **Reminder:** the engine and its milestones are still verified only against
> the desktop offline renderer (unit tests + WAV/CSV render) — the Daisy
> firmware shell that runs the actual synth on hardware is milestone **M6**.
> The CPU budget, though, has now been measured on real hardware: see
> `docs/bench/`.

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
| **M4.8** | Reverb dry/wet — equal-power MIX at the master join + clear-on-sleep CPU bypass | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **M4.9** | Reverb DIFFUSION knob (replaces DEPTH) — room density 0–0.9, weak line-mod coupling, full-wash first pass | ✅ **done** (engine + host; UI wiring deferred to M6) |
| **SYNC/COUPLE redesign** | One global SYNC switch (replaces per-part sync toggles), transport phase + rate ladder, zoned COUPLE (texture-only in grid world, grid-gravity zone in free world), VCV panel layout A, CLK/RST wired | ✅ **done** (engine + VCV host; spec `docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md`) |
| **M4.10** | Chord layer — COLOR knob, diatonic stacks, voice-leading, live FLOW surface | ✅ done (engine + hosts; hardware placement deferred to the reduction round) |
| **+ COLOR-MOTION** | MOTION becomes COLOR's third destination — bipolar additive with a zero-gate, density varies per note | ✅ **done** (engine only; no new surface) |
| **Bench** | Bench firmware — DWT cycle measurement of the engine, nine DaisySP candidates and SRAM-vs-SDRAM buffer access on real hardware | ✅ **done** (`bench/`, results in `docs/bench/`) |
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
  parts' master rates: mutual phase pull (no phase jumps) with a ±5-octave
  ([1/32..32]) rate clamp, locking in 1–2 cycles at couple = 1 and staying
  quiet at low couple. Superseded by the SYNC/COUPLE redesign below: the
  per-part `sync_mode` anchor is gone, replaced by a single global SYNC
  switch and zoned grid gravity / hard lock against the transport.
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

### M4.8 — Reverb dry/wet mix ✅

- `set_reverb_mix` (0..1): equal-power dry/wet crossfade at the master join —
  dry = cos(m·π/2), wet = sin(m·π/2) with exact endpoints, 10 ms one-pole
  glide. Default 0.25, chosen by ear: keeps the dry level with a leaner room
  than the old fixed mix (wet −8.3 dB; the old balance sits at MIX 0.5,
  −3 dB overall — MIX multiplies on top of the internal wet trim). The wet
  path keeps its internal −8 dB bloom-headroom trim; the send input is
  untouched by MIX, so the tail character never changes while turning.
- MIX 0 is a true bypass: the wet gain fades out, the room is cleared once
  (`AmbientReverb::clear()` — buffer + loop filter state, params survive) and
  `process()` is skipped. Oliverb CPU drops to zero; a self-oscillating bloom
  is genuinely killed. Any MIX > 0 wakes into a clean, empty room
  (`reverb_asleep()` exposes the gate for the M6 UI).
- Hosts: VCV `REV_MIX` knob (shared center strip, default 0.25), render
  action `set_reverb_mix`.

### M4.9 — Reverb DIFFUSION knob ✅

- `set_reverb_diffusion` (0..1) replaces `set_reverb_depth`: AP coefficient
  `0.90·n` (0 = discrete slap echoes, boot 0.7 → 0.63 ≈ the old stock 0.625
  room, 1.0 = dense wash that melts attacks), line modulation weakly coupled
  (`(0.05 + 0.20·n)·450` samples — motion rides the knob, never dominant).
  DEPTH is gone ersatzlos, like shimmer in M4.5.
- Motivation: at full MIX/DECAY/SIZE the Oliverb feeds the freshly diffused
  input straight to the output taps, so attacks punched through the wash;
  more diffusion smears the first pass (A/B verified by ear, 2026-07-14).
- Hosts: VCV `REV_DIFF` "DIFF" knob (same panel slot/param id as DEPTH),
  render action `set_reverb_diffusion`; `ambient_wash` migrated.

### SYNC/COUPLE redesign ✅

Landed 2026-07-16 (spec: `docs/superpowers/specs/2026-07-16-sync-couple-redesign-design.md`,
plan: `docs/superpowers/plans/2026-07-16-sync-couple-redesign.md`). One
global **SYNC** switch replaces the two near-invisible per-part `Free / Sync /
Triplet` toggles and gives COUPLE a clear split between "grid world" and
"organic world":

- **Transport phase** — new beat/bar phase accumulator in the engine core,
  advanced from tempo BPM at control rate; `Instrument::clock_pulse()` lets a
  host report external CLK edges (re-measures BPM and aligns downbeat phase,
  previously rate-only), and RST is now actually read and reset-aligns the
  phase.
- **Rate ladder** — 17 speed-sorted musical divisions (`engine/mod/divisions.h`)
  replace the old 9-division per-part Sync table; SYNC-on RATE snaps to the
  ladder, SYNC-off RATE stays continuous 0.02–30 Hz.
- **Grid world (SYNC on)** — both parts' PITCH lanes servo-lock to the
  transport (rate + downbeat phase); COUPLE governs only the four non-pitch
  mod lanes (1.0 lockstep, lower = independent Kuramoto breathing); DRIFT's
  rate tap likewise skips the pitch lane, its detune tap stays global.
- **Organic world (SYNC off)** — unchanged pairwise Kuramoto below COUPLE 0.5;
  0.5–1.0 fades in "grid gravity" (the coupled target additionally pulled
  toward the nearest musical division of tempo, rate and phase); COUPLE = 1.0
  converges to the same hard lock as grid-world SYNC, so flipping SYNC at full
  COUPLE is seamless.
- **VCV host** — panel layout A: TIME group (SYNC / TEMPO / COUPL) under
  MORPH, per-part sync toggles removed, CLK phase-align + live RST wired,
  division-aware RATE tooltip. Param relayout breaks saved 2.0.x patches
  (expected — version bump to **2.1.0**).
- Full engine suite green (216 cases, 0 skipped) and both `ambient_wash` /
  `demo_step_melody` renders verified clean post-landing; VCV Rack play test
  and audio listening pass deferred to a human (see dev log).

### M4.10 — Chord layer (COLOR knob) ✅

Spec: `docs/superpowers/specs/2026-07-17-chord-layer-color-design.md`, plan:
`docs/superpowers/plans/2026-07-17-chord-layer-color.md`. One new per-part
knob, **COLOR** (0..1), turns the single-note engine into a chord instrument
without a mode: 0 is today's one note, higher settings add tones (fifth-below,
then root/third, then seventh, then a ninth color tone at full), zones voiced
additively and crossfaded with hysteresis so the knob never flutters on an
edge.

- **Engine** — chord tones are built from the active scale's quantizer mask
  (diatonic stacking: root + every second scale note), so chord quality is
  emergent from the scale, never selected, and always harmonizes with the
  other part's melody. Voice-leading picks the chord lay that minimizes
  total semitone movement from the previous chord (common tones stay put).
  Per-note gain scales ~1/sqrt(n) so density changes color, not level.
- **Live surface** — in FLOW, COLOR acts continuously on the sounding voices
  (bloom in / collapse out, click-free) rather than latching at the next
  trigger; in STEP the chord is built at trigger time. `Instrument::set_color(int,
  float)` is the host entry point.
- **Hosts** — VCV `COLOR_A`/`COLOR_B` big knobs (panel: free corner between
  the macro orbit and the center strip), render action `set_color`
  (`chord_bloom.json` demo scenario). Default 0 keeps the init patch's
  single-note sound bit-identical.
- Full engine suite green, zero pre-existing failures; the three Task 1
  baseline scenario hashes match post-landing (COLOR-0 bit-identity proven);
  `chord_bloom.json` renders deterministically. VCV Rack play test and
  audio listening pass deferred to a human. Hardware panel placement is
  explicitly deferred to the upcoming reduction/macro round (per the
  standing hardware-reducibility constraint).

### COLOR as a MOTION target ✅ (extends M4.10)

Spec: `docs/superpowers/specs/2026-07-18-color-motion-target-design.md`, plan:
`docs/superpowers/plans/2026-07-18-color-motion-target.md`. COLOR was the one
pitch-layer macro nothing could modulate — a stab in a phrase always carried
the same chord density. MOTION becomes COLOR's third destination, alongside
the pan fan and drift amount it already drives, so density now varies per
note instead of tracking wherever the knob was last left.

- **Engine** — bipolar additive with a zero-gate, not multiplicative: `Part`
  now owns the COLOR knob and adds MOTION's ±1 output, scaled by MOD and a
  `kColorMod` constant (0.2), gated in over the first 1% of knob travel
  (`kColorGate`). In STEP each trigger samples whatever density is current at
  that instant; in FLOW the existing zone-hysteresis path reads a moving
  color as a bloom/collapse. `COLOR = 0` forces the gate to 0 and `MOD = 0`
  zeroes the swing, so both invariants hold structurally rather than by
  tuning: the chord layer's bit-identity guarantee and today's-behaviour
  default survive untouched.
- **No new surface** — no panel control, no scenario action, no parameter id;
  disabling works through MOTION's existing target-active flag.
- The three COLOR-0 chord-layer baselines (`ambient_wash`, `demo_step_melody`,
  `demo_density_sweep`) render byte-identical pre- and post-landing;
  `chord_bloom.json` sweeps COLOR to 0.95 at the boot MOD of 1.0 with MOTION
  active, so its reference render was re-cut to the now-breathing chords.

### Bench ✅

Plan: `docs/superpowers/plans/2026-07-18-bench-firmware.md`. `bench/` is a
standalone Daisy app, never shipped and never linked into `spotykach.bin` —
it boots the engine alone on a Daisy Seed and reads DWT cycle counts around
fixed workloads, then prints a Markdown/CSV pair over semihosting. The
shipping firmware (`main.cpp`, `app.cpp`, `src/`, `engine/`, the root
Makefile) is untouched by its presence; Step 1 of the bench plan re-proves
that on every run.

The headline numbers are no longer estimates — they come from a real Daisy
Seed at 480 MHz, 48 kHz, block 96 (`docs/bench/2026-07-19-87f3538.md`):

- The full instrument at its worst case (8 voices, COLOR 4-note on both
  parts, all FX on, high diffusion, echo at max) costs 91 % (avg) /
  96 % (max) of the block budget offline and 91 % (avg) /
  96 % (max) anchored inside a real audio callback. **The max is under budget
  for the first time, and the max is the gate** — so the bench now emits *"the
  2×4 architecture fits"* on its own. Four optimization passes have taken ~60
  points off the anchored max, from 156 %. The margin is **4.2 points**, which
  is thinner than the saving the last cut returned from its larger call site:
  one unbudgeted feature can spend it. The newest pass is the **fast-tanh cut**
  (spec `docs/superpowers/specs/2026-07-19-fast-tanh-design.md`), worth
  **8.1 points** on the anchored max (103.89 % → 95.77 %) against a predicted
  ~11 — **it underdelivered, and cleared the gate only because just ~4 points
  stood in the way.** Before it, the **mod-plane control-rate cut**
  (`docs/superpowers/plans/2026-07-19-mod-plane-control-rate.md`), worth
  **~19 points** against a predicted 17–19: the plane fell 253 254 → 56 667
  cycles (−77.6 %), far past the spec's own expectation. Before it, on the
  same branch, the **Part-glue control-rate cut**
  (`docs/superpowers/plans/2026-07-19-part-glue-control-rate.md`) was worth
  ~19.6 points — the glue fell 112 820 → 18 664 cycles per part, 83.5 %,
  against a predicted 70–85 % — alongside four **FX hygiene cuts** whose
  largest more than halved the reverb (`oliverb_solo_sram` 186 673 → 91 420
  cycles). What is left on the ranked list below is no longer needed to clear
  the gate; it should be held as margin rather than spent.
- **The drifted attribution is re-baselined and the flag is cleared — but the
  family predicts rank order better than magnitude.** `part_glue_flow` had
  halved a second time at `94468af` (19.86 % → 9.97 %) even though the
  Part-glue cut had already landed at `c7f6a73`, which looked like instability.
  At `87f3538` it reads 9.98 % — a 0.07 % move across an independent build,
  same checksum. Two consecutive runs agree, so that second halving was a
  one-time re-attribution when the 96-sample raster tick landed, not drift.
  Every row the fast-tanh cut should not have touched held (`grit_drive_solo`
  identical to the cycle, `synth_4_voices` within 1 cycle, the `micro_*`
  controls flat), and the checksum column confirms only FLUX and driven-limiter
  rows changed hash. The family can be trusted again — with one lesson from
  spending it: its two fast-tanh predictions were **both high** (8 → 5.8, 3 →
  1.5), because a ceiling books a whole call site's cost to the one call it
  contains. Treat the ranking as reliable and the absolute figures as upper
  bounds. Two further cautions: an `inst_worst_no*` difference carries a ~10 %
  composition-and-layout error band (in-context reverb moved 11 289 cycles with
  no reverb code change), and a solo-row saving is an upper bound on what the
  composed instrument returns (FLUX gave back 3.56 points in context against
  the 5.80 its solo rows predicted).
- Two caveats on the recent runs. `echo_short_sram` / `echo_short_sdram` are
  **not comparable** to `9be5df9` — the delay-time one-pole moved out of
  `EchoDelay` into `Flux`, so those rows no longer carry the per-sample
  slew (they are comparable `94468af` → `87f3538`, which is where the
  fast-tanh halving above is read). And the earlier figures below (the
  ablation closure, the mod-plane history) are stated against `9be5df9` and
  were not re-derived since.
- **The unaccounted gap is now attributed, and the go/no-go conclusion did
  not move.** Component rows summed to ~120 % of budget while
  `instrument_worst` measured ~159 % (avg) — a ~375k-cycle (39-point) gap
  with no named owner. Fourteen `abl` bench rows
  (`docs/superpowers/plans/2026-07-18-bench-ablation-family.md`) close it on
  paper: **Part glue** — `Part::process`'s per-sample lane-target/quantizer/
  ChordBuilder machinery, isolated for the first time — is the single
  largest owner at ≈112820 cycles/part (≈12 % of budget each, ≈23 % for
  both parts); the **driven master limiter** costs 27698 cycles (≈3 %)
  whenever `MASTER DRIVE` defeats its bit-exact bypass; running the
  **reverb** in-context costs 42076 cycles (≈4 %) more than its isolated
  cost — a real composition/cache-coupling tax that FLUX's own coupling
  term does *not* show (that one came back negative); and **CHOKE**, once
  actually measured instead of assumed, *reduces* worst-case cost by
  ≈94293 cycles (≈10 %) — it is not a worst-case axis. Inside FLUX,
  `std::tanh` in `EchoDelay` is now confirmed the dominant per-sample cost
  (≈60 % of FLUX's isolated per-part delta over the FX-none shell (fx_flux_sdram − fx_none)), ahead of its
  SDRAM memory tax (≈5 %) and its remaining bpf/interpolation/SetDelay
  machinery (≈35 %). Summing every named term (mod plane ×2, Part glue ×2,
  engine ×2, full PartFx ×2, in-context reverb, driven-limiter tax) against
  `instrument_worst` closes to within 7.8 % of budget (5.2 % of
  `instrument_worst`) — under the 10-point threshold, so the residual is not
  treated as a missing owner; it's attributed to the additive-stacking
  approximation used for "full PartFx" (no row yet measures GRIT+FLUX+COMP
  running together in one part) plus compounded row-to-row jitter. The 2×4
  verdict did not move *at the time* — at `9be5df9` `instrument_worst` still
  sat within jitter of every prior measurement — the closure's contribution
  was naming where the cost lives instead of leaving 39 points dark; it is
  the four cuts it enabled that eventually flipped the verdict, four commits
  later. Ranked cut list for the next spec
  (predicted savings, largest first, all as % of the 960k-cycle budget):
  Part glue to control rate — **SPENT 2026-07-19** (`c7f6a73`, spec
  `2026-07-19-part-glue-control-rate-design.md`): measured **19.6 %**, not the
  23 % ceiling, because 18 664 cycles/part are a mandatory per-sample
  remainder. The `SMOOTH=0` risk case largely dissolved — the engine had
  never seen those intermediate values, since it reads at its own 96-sample
  control tick; what remained was fire timing, answered with an event refresh
  rather than a finer raster; reverb composition-coupling investigation (≈4 % already paid,
  mechanism unexplained) plus a speculative, unmeasured half-rate-reverb
  hypothesis (order ≈10 %, needs its own ablation before it's trusted); fast
  tanh in `EchoDelay` — **SPENT 2026-07-19** (`87f3538`, spec
  `2026-07-19-fast-tanh-design.md`): measured **5.8 %** against a ceiling of
  ≈8 %, the echo kernel itself more than halving (`echo_short_sram` 21 154 →
  8 752 cycles) but returning less once composed into the instrument; fast tanh
  in the master limiter's `shape()` — **SPENT 2026-07-19**, same commit:
  measured **1.5 %** against ≈3 %, half the prediction, because the ceiling had
  booked the whole driven-limiter tax to `tanh` when most of what remains is
  gain-riding arithmetic. Together **8.1 points** on the anchored max, which
  cleared the 100 % gate; and two hygiene
  one-liners already known from source — `PartFx` rev-send `std::sin` →
  `fast_sin` (measured ≈1 %, ceiling ≈2 %) and the double pitch
  quantization in `Part::process` (ceiling ≈2 %, likely much smaller, no
  dedicated bench row yet) — both change engine output and belong with a
  listening pass, not a silent merge. Full arithmetic and the complete
  ranked list:
  `docs/superpowers/plans/2026-07-18-bench-ablation-family.md`, `## Outcome
  (2026-07-19)`.
- The modulation plane was originally measured at about 33 % of the block
  budget against the design spec's 4–6 % estimate — wrong by roughly six
  times, and the single most actionable finding in the table. The cause,
  found by decomposing the plane into per-lane bench rows (plan+spec
  `docs/superpowers/specs/2026-07-18-mod-plane-optimization-design.md`):
  `waveforms.h`'s `wave_sine` called libm `std::sin` once per sample per
  lane, even though the audio path itself had used the cheap `fast_sin`
  polynomial since M2 — the modulation path had simply never been moved
  over. Switching it to `fast_sin` brought the plane down to about **26 %**
  of the block budget; still measured from AXI SRAM rather than the firmware's
  zero-wait DTCMRAM, so the figure stays conservative. The spec's own prediction (a fall to about 17 %)
  **fell short** — the sine really was the single biggest line item, but
  its estimated per-lane cost was too high, so the realized saving came in
  at about 40 % of what the spec predicted. A smaller residual cost
  (`fast_sin` itself running closer to ~50 cycles than the ~10–15 its own
  header claims on this call site) and the ten lanes' own per-sample
  machinery — independent of any waveform call — now account for most of
  what is left; see the spec's Outcome section for the full breakdown —
  DONE 2026-07-19 (texture lanes on the 96er raster, spec
  2026-07-19-mod-plane-control-rate-design.md) — **SPENT, measured at
  `94468af`: the plane fell to 5.90 % (avg) / 6.11 % (max) of the block
  budget, 253 254 → 56 667 cycles, a 77.6 % cut worth ~19 points on the
  anchored worst case (predicted 17–19).** Where the earlier `wave_sine`
  cut undershot its spec, this one overshot: moving the lanes off the
  per-sample path removed not just their waveform evaluation but the
  per-sample lane machinery that the previous Outcome had identified as the
  irreducible remainder. `super_mod_5lanes` fell 13.23 % → 2.86 % on the
  same run.
  The mod plane's output changed deliberately as part of this cut (`fast_sin`
  is not bit-identical to libm `sin`), so `renders/` byte-identity is no
  longer treated as a regression gate for it — re-cut references are the
  accepted price, per the spec's own decision.
- The grain-read proxy — the access pattern M5's granular engine will lean
  on — costs about 5.3× in SDRAM against the same reads in SRAM. That is
  the sampler's exposure, measured before the sampler exists. **Caveat**
  (carried from the texture-deck spec): this is a directional floor-risk
  number measured over a 64 KB window, not a constant to carry forward —
  and it is a lower bound twice over, because the RNG draw and division per
  grain are common to both the SRAM and SDRAM rows and dilute the ratio,
  and a 64 KB SDRAM window is partly cached.

Numbers, method and the full nine-candidate DaisySP table live in
`docs/bench/`; how to run the bench yourself is in `bench/README.md`.

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
