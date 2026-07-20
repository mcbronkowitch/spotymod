# Spotykach Sampler Engine Adapter — M5 Design

> **SUPERSEDED (2026-07-18) — do not implement.** M5 was re-brainstormed
> after the melody rework (M4.7), the groove engine, CHOKE, the chord
> layer and the VCV plugin changed the instrument. The current spec is
> `docs/superpowers/specs/2026-07-18-sampler-texture-deck-design.md`
> **in the fork** (spotymod repo): the sampler is now a texture deck
> (granular cloud, chord-locked PITCH, MOTION scatter macro, groove-gated
> STEP bursts) built as a fresh grain scheduler plus the copied `Buffer`
> record core. The slice-player trigger model below is dead; the record
> core, memory injection, 42 s sizing and WAV plumbing carried over.

The second sound engine of the modulation-first firmware: the original
Deck/Vox granular sampler returns as a `spky::IPartEngine`, driven by the
SuperModulator lanes instead of the old track sequencer. Extends the main
design (`2026-07-10-spotykach-modulation-first-synth-design.md`, sections
"Engine 1 — Sampler" and "Sampler transport"); milestone **M5**, after M4
(center section), before M6 (firmware shell).

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Each part can switch to a granular sampler with the fixed target slots
POSITION / SIZE / PITCH / SHAPE / LEVEL, record from the inputs, and play
its buffer as lane-triggered slices (STEP) or as a free-running grain
stream (FLOW). The sound-defining code of the original firmware — `Vox`
grain playback and the `Buffer` record state machine — is reused verbatim
in behavior; the management layer is new and speaks the spky architecture.
Fully testable on the desktop; all gestures (ALT+PLAY arm/switch, LED fill
display) are M6 — engine API now, as established in M2/M3/M4.

## Decisions (from brainstorming, 2026-07-12)

- **Reuse depth: Generator+Vox+Buffer core.** The granular core (`Vox`,
  `Buffer`, `Window`, `ADEnvelope`, `XFade`) is reused; `Deck`'s track
  sequencer, detector, dispatcher, FX and loop/tempo quantization are not —
  the spky architecture already owns triggering (lanes, capture sequencer),
  FX (M1.6) and clock.
- **Approach A: thin `SamplerEngine`, Generator absorbed.** The `Generator`
  class is not copied; its live role (start/size/pitch distribution, vox
  rotation) is rewritten inside the new `SamplerEngine`, taking the mapping
  math from `generator.cpp` as the template. Its dead weight in the spky
  world (cue points, `Event` structs, start-offset intervals, MIDI mod
  flags) stays behind.
- **Code layout: copy & adapt.** `Vox` and friends are copied into
  `engine/sampler/` under `namespace spky` with their own constants header.
  The original firmware (`src/core`) stays untouched and buildable — it is
  the frozen reference; divergence of the copies is accepted.
- **FLOW = free-running grain stream.** The granular counterpart of the
  synth drone: voxes regenerate themselves continuously, lanes shape the
  standing texture, external triggers are ignored.
- **Transport scope: full record + WAV in M5.** Engine-side record
  start/stop with the original 4 ms fades, overdub, and buffer-fill query,
  plus WAV loading in the render host. The engine holds no arm state —
  arming is a gesture (M6). M6 only wires gestures/LEDs.
- **Speed modes: both, Tape default.** Tape couples speed and pitch
  (varispeed, the sampler's tape-warm identity); Digital repitches grains
  at unchanged slice speed. `set_speed_mode()` on the engine; panel
  assignment is decided in M6.

## Architecture

New directory `engine/sampler/` (platform-independent, no `src/` includes):

| File | Contents | Origin |
|------|----------|--------|
| `sampler_config.h` | constants: `kRecordFade` 192 (4 ms), `kWindowSlope` 960 (20 ms), `kMinimumWindowSize` 1920, `kDefaultWindowSize` 2880 (60 ms), `kSliceSlope` 192 (4 ms), `kDefaultFeedback` 0.95 | extracted from `src/core/config.h` |
| `sample_buffer.h/.cpp` | record buffer: fade-in/sustain/fade-out state machine, cut, overdub feedback write, fill query | copy of `src/core/buffer.h/.cpp` (+ the `SoftSwitch`/`XFade` pieces it needs), `namespace spky` |
| `vox.h/.cpp` (+ `window.h`, `grain_env.h`) | grain playback: windows, slopes, tape/digital increments, spread jitter | copy of `src/core/vox.*`, `window.h`, `adenv.h`; behavior unchanged |
| `sampler_engine.h/.cpp` | `IPartEngine` implementation: vox rotation, target mapping, transport, FLOW stream | new (mapping math taken from `generator.cpp`) |

**Integration:**

- `engine/parts/engine_iface.h`: `EngineId` gains `ENGINE_SAMPLER = 2`;
  one new default no-op virtual `void process_in(float inL, float inR) {}`
  (see Transport). TestTone and Synth ignore it.
- `Part` gains a `SamplerEngine _sampler` member; `_engine_for()` maps the
  new id. The click-free engine switch from M2 (SoftSwitch fade-out → swap
  → fade-in) applies unchanged. Sampler edit-layer setters are forwarded
  directly (like the synth VOICE edits), so edits stick while another
  engine is active.
- **Memory is injected** (spec "no heap"): `FxMem` grows by
  `SampleFrame* sampler_buf[PART_COUNT]` and `size_t sampler_frames`
  (`SampleFrame` = the copied stereo frame struct). Size follows the
  original: **42 s stereo per part** at 48 kHz (~16 MB/part). Desktop:
  static arrays in the host; M6: SDRAM, the exact pattern of the old
  `SDRAMBuffer`. `nullptr` → the sampler part runs empty and silent, no
  crash (spec error behavior).
- **Determinism:** `Vox`'s grain jitter RNG is seeded per part from the
  same seed base the synth uses; identical scenarios render bit-identically.

## Trigger model

**STEP.** Every lane trigger (`trigger(pitch_norm)`) launches a **slice**
on the next vox, round-robin over the 3 voxes — up to 3 overlapping
slices, the granular counterpart of the synth's 4-voice allocation.
Latched at trigger time: start position (POSITION), slice length (SIZE),
pitch ratio (quantized, per vox — like synth PITCH latching). Continuous
while a slice runs: SHAPE (window shape breathes live on all active
grains) and LEVEL. A slice ends after SIZE with its 4 ms slope; a fourth
trigger steals the oldest vox with a fade (M2 voice-stealing behavior).

**FLOW.** `set_flow(true)` starts the free-running texture: when a vox's
grain ends it immediately regenerates, reading POSITION / SIZE / PITCH
**continuously** (position wanders audibly, size breathes, pitch glides
quantized). The 3 voxes start phase-staggered by ⅓ window length so the
stream does not pulse. External triggers are ignored in FLOW — the synth
drone rule. `set_flow(false)`: running grains decay, then silence until
the next STEP trigger.

**Empty buffer:** triggers and FLOW are no-ops (silence, no crash). The
capture sequencer (M3) needs nothing sampler-specific — it fires the same
triggers into the same interface (engine-agnostic, per the M3 spec).

## Target mapping

| Slot | Target | Mapping |
|------|--------|---------|
| 1 | POSITION | 0..1 → start frame in `[0, recorded length)`. STEP: latched per slice. FLOW: read continuously at each regeneration. |
| 2 | SIZE | exponential **20 ms → 3 s** (`0.02 · 150^n` s), clamped to recorded length. Covers click granular through long phrases. |
| 3 | PITCH | identical path to the synth: target + TUNE → quantizer → semitones. The engine converts to a ratio `2^(semi/12)`. **Tape** (boot default): playhead increment = ratio — speed and pitch coupled, varispeed. **Digital**: playhead 1×, grain windows read at the ratio — pitch without time change (the two `Vox` increment paths). |
| 4 | SHAPE | 0..1 → grain window shape via `Vox::set_shape` (attack/decay skew), continuous on all running grains. Window size stays at the 60 ms default (edit layer below). |
| 5 | LEVEL | master gain through a `OnePole` (10 ms) — exactly as the synth. |

**Edit layer** (the sampler's counterpart of the synth VOICE edits;
gesture assignment is M6, `Instrument` API now): `set_win_size` (window
size, 40 ms minimum), `set_feedback` (overdub feedback, default 0.95 ≈
−3 dB), `set_reverse` (reverse playback), `set_speed_mode` (Tape/Digital).

## Transport, recording, input path

**Input path (new plumbing).** `Instrument::process` currently discards
its inputs. M5 threads them through: `Part::process` gains input samples
and calls the engine's `process_in(inL, inR)` before `process`;
`Instrument::process` passes `in[i]` per sample. Only the sampler engine
implements `process_in` (it writes to its buffer while recording).

**Input monitoring** (added 2026-07-12, cross-spec review): while
monitoring is enabled the sampler engine also passes the dry input
through to its output at unity — into the part chain pre-GRIT, so the
player hears the source in context. API: `set_monitor(bool)` on the
engine / `sampler_monitor(p, bool)` on `Instrument`. The shell (M6)
enables it automatically while the part is armed or recording, disables
it otherwise — no user toggle. Without this, live sampling is blind: a
fresh recording into an empty buffer produces no grain output until it
stops, and the analog input trimmers are invisible to firmware, so the
record level could never be set by ear.

**Recording (engine side complete in M5):**

- `set_recording(bool)` starts/stops capture from the `process_in` samples
  with the original 4 ms fades (buffer state machine
  `fadein → sustain → fadeout`, copied unchanged).
- Recording into an empty buffer defines the content length — free length,
  **no loop quantization** (the sampler is a granular source here, not a
  looper; rhythm is the capture sequencer's job). Buffer full (42 s) →
  auto-stop with fade.
- Recording while playing = **overdub** (feedback write, default −3 dB).
- Queries for host/LEDs: `is_recording()`, `buffer_fill()` (0..1),
  `is_empty()`; plus `clear()`.
- `Instrument` API: `set_engine(p, ENGINE_SAMPLER)` (exists),
  `sampler_record(p, bool)`, `sampler_clear(p)`, `sampler_fill(p)`,
  `load_sample(p, l, r, frames)`, and the edit-layer setters
  (`sampler_win_size`, `sampler_feedback`, `sampler_reverse`,
  `sampler_speed_mode`).

**WAV loading (desktop).** `Instrument::load_sample` copies into the
injected buffer and sets the content length. The render host gains a small
WAV reader (counterpart of the existing writer) and two scenario
extensions: action `load_wav {part, path}` and an optional top-level field
`input_wav` that feeds the `process` inputs — live-recording scenarios are
testable end-to-end. M6 replaces only the source (SD card instead of disk)
through the same `load_sample` entry point.

**Loading semantics** (added 2026-07-12, cross-spec review): loading
implies `clear()`, and the content length grows as chunks arrive — the
record path's fill semantics, reused. On the Daisy a 42 s stereo file is
~16 MB of chunked SD reads through the main loop; with fill-follows
semantics "playing while loading" is defined for free (the engine
granulates only the already-valid region) instead of granulating
half-overwritten content.

## Testing

Unit tests on the desktop against `engine/`, TDD as established:

- **Record fades:** starting/stopping recording produces no discontinuity
  (max sample delta across the fade boundaries).
- **Latching:** changing POSITION/SIZE mid-slice does not move the running
  slice in STEP; in FLOW it applies at the next regeneration.
- **Tape pitch:** 440 Hz sine in the buffer, PITCH +12 semitones →
  measured frequency ≈ 880 Hz (zero-crossing count, M2 pitch-test
  tolerance).
- **Digital pitch:** same measurement, slice duration constant within ±1
  window length.
- **FLOW continuity:** a standing stream over several seconds — RMS never
  drops below a threshold in any 50 ms window (no pulsing, no gaps).
- **Vox stealing:** a fourth trigger steals the oldest vox click-free.
- **Empty behavior:** empty buffer and `nullptr` memory injection →
  silence, no crash.
- **Determinism:** two identical renders are bit-identical (seeded
  jitter).
- **Engine switch:** synth ↔ sampler click-free (M2 switch test pattern
  extended with `ENGINE_SAMPLER`).

**CPU:** 3 voxes × 2 parts = 6 grain readers (linear-interpolated stereo)
plus window math — expected well under the synth budget. Measured with the
existing host bench; the number goes into the milestone notes as a
checkpoint, not a risk.

## Acceptance demo

Portfolio render: part A = synth, part B = sampler. `input_wav` plays in,
B records live, then granular decomposition — a STEP phase with
polyrhythmic slices, a switch to a FLOW texture with wandering POSITION,
PITCH quantized in the active scale. Output WAV + CSV (buffer fill, 3
playheads, 3 envelopes per part) for plots.

## Roadmap placement

Milestone **M5 — sampler adapter**, after M4 (center section), before M6
(firmware shell + hardware mapping), as in the main design's build order.

## Assumptions to verify during implementation

- `Vox`/`Buffer` desktop portability: confirmed at header level (no
  libDaisy includes); verify at link time when the copies first build in
  the render host.
- `std::normal_distribution` (grain jitter) is implementation-defined
  across standard libraries — for bit-identical renders across platforms,
  replace it in the copy with the engine's own noise source (same one the
  lanes use) during adaptation.
- The 42 s buffer assumes the M6 SDRAM map has room next to the FX delay
  buffers — recheck against the old `SDRAMBuffer` layout when M6 allocates
  (the original fit three 42 s buffers plus delays, so two is safe).
- SIZE's 3 s ceiling vs. `Vox`'s internal `kMaxSpread` (3 s at 1×):
  confirm the copied code has no hidden coupling between spread and slice
  length at extreme SIZE × PITCH combinations.
