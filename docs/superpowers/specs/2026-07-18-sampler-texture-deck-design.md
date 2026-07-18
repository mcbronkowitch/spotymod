# M5 Sampler — the texture deck

**Date:** 2026-07-18
**Status:** approved (brainstorm with Bastian, 2026-07-18)
**Supersedes:** the residency-repo spec
`2026-07-12-spotykach-sampler-adapter-design.md`. That spec was written
before the melody rework (M4.7), the groove engine, CHOKE, the chord layer
and the VCV plugin existed; its slice-player trigger model no longer matches
the instrument. Its proven pieces (Buffer record core, memory injection,
42 s sizing, WAV plumbing) carry over; everything else is re-decided here.

## Identity

The sampler is **not** a second melodic instrument. It is a **texture deck**:
a granular cloud fed from whatever comes in — the synth part, external gear,
a loaded file. The synth part makes the music; the sampler makes the room.

Decisions from the brainstorm:

- **Role: granular cloud**, FLOW-centric. Melody is secondary (but the cloud
  stays harmonically locked to the instrument, see PITCH).
- **Material: live input first, WAV second.** IN L/R recording is the
  primary path (resampling the own synth part via a patch cable is a
  first-class use case); file loading is the second path.
- **PITCH: quantized + chord cloud.** The cloud always plays in the
  instrument's scale, and with COLOR > 0 it spreads across the current
  chord — a harmonic shimmer over the device's harmony.
- **MOTION: scatter macro**, one axis from focused loop to diffuse fog.
- **STEP: groove-gated bursts** — the phrase generator chops the texture in
  composed rhythm. FLOW: the standing cloud.
- **Panel: existing ENG pad + one new REC button per part.** No further
  surface; the edit layer lives in the context menu.
- **No dead knobs (critical-review addendum, 2026-07-18):** the voice row
  (ATK DEC FILT RES SUB DTUN) is remapped to analogous cloud meanings —
  every printed label stays true for both engines. The cloud granulates
  the already-captured region *while* recording, and a factory sample
  auto-loads when a part switches to the sampler with an empty buffer —
  one pad press and it sounds, nothing to configure.
- **Architecture: fresh grain scheduler + copied record core.** The player
  is written natively for cloud/chord/scatter/gating semantics; the
  battle-tested `Buffer` record state machine is copied, not rewritten.
  `src/core/vox.*` serves as math reference (interpolated read, window
  slopes), not as code.

## Architecture

New directory `engine/sampler/` — platform-independent, no libDaisy, no
heap, injected memory, own seeded `Rng` (the established engine rules):

| File | Contents | Origin |
|------|----------|--------|
| `sampler_config.h` | constants: `kRecordFade` 192 (4 ms), `kDefaultFeedback` 0.95, `kGrains` 8, `kBurstRelease` (~60 ms), scatter ranges, size curve | new (record constants from `src/core/config.h`) |
| `sample_buffer.h/.cpp` | record buffer: fadein→sustain→fadeout state machine, overdub feedback write, cut, fill/empty queries, clear | copy of `src/core/buffer.h/.cpp` + the `SoftSwitch`/`XFade` pieces it needs, `namespace spky` |
| `grain.h` | one grain: latched start/ratio/pan/window, interpolated stereo read, Hann window with skew, forward/reverse | new (`vox.cpp` as math reference) |
| `sampler_engine.h/.cpp` | `IPartEngine`: scheduler, chord distribution, scatter, gating, transport | new |

`src/` stays untouched and buildable — the frozen reference.

### Integration

- `engine/parts/engine_iface.h`: `EngineId` gains `ENGINE_SAMPLER = 2`.
  Two new default-no-op virtuals (precedent: `set_hold`):
  - `void process_in(float inL, float inR)` — per-sample input feed; only
    the sampler implements it (records / monitors).
  - `void set_gate(bool)` — `Part` forwards its composed gate signal
    (manual 5 ms pulse OR'd with the groove's note sustain, exactly what
    `Part::gate()` already computes) so the cloud can sound for the
    composed note duration in STEP. The synth ignores it (has its own env).
- `Part` gains a `SamplerEngine _sampler` member; `_engine_for()` maps the
  id. The M2 click-free SoftSwitch engine switch applies unchanged. Sampler
  setters are forwarded while another engine is active (edits stick, like
  the synth VOICE edits).
- `Part::process` gains the input samples and calls `process_in` on the
  active engine before `process`; `Instrument::process` finally threads its
  (currently discarded) `in` pointers through.
- `set_hold(bool)` (CHOKE): while held the cloud stops spawning grains and
  running grains fade out (click-free); release re-arms. In FLOW the
  sampler counts as a drone — `Part::flow()` semantics apply unchanged, a
  priority sampler part in FLOW holds its choke window like a synth drone.
- **Memory is injected:** `FxMem` grows by `SampleFrame* sampler_buf[2]`
  and `size_t sampler_frames`. Sizing follows the original: **42 s stereo
  per part** at 48 kHz (~16 MB/part). Hosts allocate (desktop/Rack: heap;
  M6: SDRAM — the original fit three 42 s buffers plus delays, two are
  safe). `nullptr` → the sampler part runs silent, no crash.

## The cloud (player)

**8 grains per part.** Each grain latches at spawn: buffer start position,
pitch ratio, pan position, window length/skew, direction. Playback is a
linear-interpolated stereo read under a Hann window with skew. Per-grain
gain is scaled ~1/sqrt(active grains) (the COLOR loudness lesson) so
density changes don't pump the compressor.

Scheduling runs at the control tick (per 96 samples) with sample-accurate
spawn offsets inside the block; grain start phases are staggered so the
carpet never pulses at any SIZE.

### Lane mapping

| Lane | Effect |
|------|--------|
| SOURCE | Cloud center: 0..1 → position in `[0, recorded length)`. Grains latch at spawn — the lane wanders, the cloud audibly drags behind. |
| SIZE | Grain length, exponential **20 ms → 2 s** (`0.02 · 100^n` s), clamped to content length. |
| PITCH | Same path as the synth: lane + TUNE → quantizer → semitones → ratio `2^(semi/12)`. **Chord distribution:** `Part` feeds the current COLOR notes via `set_chord` (already refreshed every sample); each spawning grain takes the next chord note round-robin. COLOR 0 = all grains on the root — behavior identical to the single-note world. |
| MOTION | **Scatter macro, order→chaos:** 0 = all grains tight on the SOURCE point, stereo-centered, regular spawn timing (loop-like); 1 = position jitter (up to ±¼ of content length), full stereo spread, spawn-timing jitter, and a mild octave scatter on chord notes. All jitter draws from the part's seeded `Rng`. |
| LEVEL | Master gain through a `OnePole` (10 ms), exactly as the synth. The LEVEL floor applies in `Part` as for every engine. |

### Tape/Digital, translated to the cloud

A cloud has no playhead — pitch and time are decoupled by construction, so
the old slice-mode distinction becomes a grain property:

- **Digital (default):** fixed output duration SIZE, the grain reads
  `SIZE · ratio` worth of material. Texture speed independent of pitch.
- **Tape:** the grain covers a fixed SIZE of material and lasts
  `SIZE / ratio` — low notes become long smearing grains, high notes short
  and fleeting. The tape character as a cloud property.

Edit layer (context menu, `Instrument` API now, hardware gesture later):
speed mode (Tape/Digital), **Reverse** (grains read backwards), overdub
feedback.

### Voice row remapped — no dead knobs

The per-part voice row is synth-only today; on a sampler part six knobs
would go dead. Instead the same knobs get analogous cloud meanings, so the
panel stays 1:1 across engines (and on hardware the VOICE edit layer is
simply reinterpreted per engine — zero new controls):

| Knob | Synth | Sampler (cloud) |
|------|-------|-----------------|
| ATK / DEC | envelope | grain-window attack/decay halves (soft↔percussive; replaces a separate skew control); DEC additionally scales the STEP burst release |
| FILT / RES | bipolar cutoff trim / resonance per voice | **one stereo `Svf` on the cloud output, same bipolar FILT semantics** — full left fades the texture into silence (LEVEL-OnePole fade, the FILT invariant), full right opens to 14 kHz; RES = its resonance |
| SUB | sub oscillator level | share of grains spawning an octave down |
| DTUN | voice detune | per-grain detune spread in cents (seeded jitter) |

`Instrument` gains the sampler-side setters (`sampler_window`,
`sampler_filt`, `sampler_res`, `sampler_sub`, `sampler_detune`); the VCV
layer pushes voice-row values to both engines every frame, as it already
does for synth edits (edits stick while the other engine is active).

### FLOW and STEP

- **FLOW — standing cloud.** When a grain ends it immediately respawns,
  reading SOURCE/SIZE/PITCH/MOTION continuously. The lanes shape the
  texture; the cloud never gaps (RMS-continuous). `set_flow(false)`:
  running grains decay, then silence until the next gate.
- **STEP — groove-gated bursts.** `trigger(pitch_norm)` /
  `trigger_chord(...)` latches the pitch (or chord set) for the burst;
  grains spawn only while `set_gate` is high, plus a short release
  (`kBurstRelease` ~60 ms, running grains decay naturally). The phrase
  generator's composed rhythm — note durations, DENSITY, MELODY variation,
  CHOKE windows, the GATE jack — all act on the texture for free.
- **Empty buffer:** triggers, gates and FLOW are no-ops (silence, no
  crash).

## Material: recording, loading, persistence

**Recording (engine side, complete in M5):**

- `set_recording(bool)` captures from the `process_in` samples with the
  original 4 ms fades. Recording into an empty buffer defines the content
  length — free length, no loop quantization (rhythm is the phrase
  generator's job). Buffer full (42 s) → auto-stop with fade. Recording
  over existing content = **overdub** (feedback write, default 0.95
  ≈ −3 dB).
- **The cloud plays while recording (fill-follows):** the growing content
  length is live — grains granulate the already-captured region as REC
  runs (SOURCE maps into the current fill). The sound *emerges under the
  gesture* instead of appearing after it; together with auto-monitoring
  this makes live sampling a performance move, not an admin step.
- **Monitoring:** while enabled, the dry input passes to the engine output
  at unity — into the part chain pre-GRIT, so the player hears the source
  in context and can set levels by ear. The host enables it automatically
  while REC is on, disables it otherwise; no user toggle.
- Queries: `is_recording()`, `buffer_fill()` (0..1), `is_empty()`;
  plus `clear()`.

**Instrument API:** `set_engine(p, ENGINE_SAMPLER)` (exists),
`sampler_record(p, bool)`, `sampler_clear(p)`, `sampler_fill(p)`,
`sampler_monitor(p, bool)`, `load_sample(p, l, r, frames)`, the edit
layer (`sampler_speed_mode`, `sampler_reverse`, `sampler_feedback`) and
the voice-row setters (`sampler_window`, `sampler_filt`, `sampler_res`,
`sampler_sub`, `sampler_detune`).

**Loading semantics:** loading implies `clear()`; the content length grows
as chunks arrive (the record path's fill-follows semantics reused). Playing
while loading granulates only the already-valid region — defined behavior
on every platform, including future chunked SD reads on hardware.

### VCV layer

- **Jacks:** IN L/R finally wired to `Instrument::process` inputs; L
  normals to R when only L is patched.
- **Panel:** the per-part **ENG pad already exists** (today it latches
  Synth ↔ Test tone). It is remapped to **Synth ↔ Sampler** — the test
  tone moves to the context menu as a dev tool (a saved patch that had
  test tone selected now opens as sampler; accepted, no real patches use
  it). Switching runs through the existing click-free path. The **only new
  panel element** is **REC** — a latching button + LED per part, appended
  LAST in PARAMS as a pair (param ids stable, saved patches compatible):
  on = record (empty buffer defines length, full = overdub), off = stop
  with fade. Monitoring follows REC automatically. REC on a synth part is
  inert (LED dark) — ENG is the only mode selector.
- **Context menu per part:** *Load sample…* (osdialog file dialog +
  vendored `dr_wav`, host-only — the engine stays clean), *Save sample…*,
  *Clear sample*, Tape/Digital, Reverse, overdub feedback (menu slider),
  *Engine: test tone* (dev) — the full edit layer, so the panel stays at
  ENG + REC.
- **Factory sample (first-user experience):** a short factory WAV ships as
  a plugin asset — a bounced spotymod synth drone (harmonically rich
  sustained material granulates well and is on-brand). When a part
  switches to the sampler **with an empty buffer**, the factory sample
  auto-loads: one pad press and it sounds, nothing to configure. It never
  overwrites recorded, loaded or patch-persisted content, and REC/load/
  clear treat it like any other content. Hardware counterpart (M6): a
  factory `A.wav`/`B.wav` on the shipped SD card — the planned boot
  autoload covers it.
- **Persistence:** the loaded WAV path goes into `dataToJson` and is
  re-loaded on patch load. Additionally `onSave` writes the current buffer
  content as WAV into the Rack patch-storage directory and reloads it on
  open — a live-recorded texture survives save/reopen. (Cheap: the dr_wav
  writer exists anyway for *Save sample…*.)

### Desktop host

Scenario action `load_wav {part, path}` (WAV reader mirrors the existing
writer) and an optional top-level field `input_wav` that feeds the
`process` inputs — live-recording scenarios are testable end-to-end. CSV
gains per-part `fill` columns.

## Determinism, errors, CPU

- **Determinism:** one `Rng` per part, seeded from the same base as the
  lanes; no `std::` random machinery (implementation-defined across
  platforms). Identical scenarios render bit-identically; the double-render
  `cmp` gate applies.
- **Neutrality gate:** a part on ENGINE_SYNTH must render bit-identically
  to pre-M5 (the input-threading and iface additions must be no-ops there)
  — the counterpart of the COLOR-0 gate, verified on the pinned baseline
  scenarios.
- **Errors:** `nullptr` buffer → silent part, no crash; empty buffer →
  no-op triggers; all edit setters safe while another engine is active.
- **CPU:** 8 grains × 2 parts = 16 interpolated stereo reads + window math
  — expected well under the synth budget on desktop. Measured with the
  host bench; the number goes into the milestone notes as a checkpoint.
  **Hardware caveat (named honestly):** grain reads are scattered SDRAM
  accesses that bypass the cache — exactly the SRAM-vs-SDRAM weakness the
  engine-expansion research surfaced (NIME source). Of all engines the
  sampler is the most exposed to SDRAM latency; it goes on the benchmark
  firmware list before the 2×4 CPU budget is committed. Desktop numbers
  do not transfer.

## Testing (doctest, TDD as established)

- **Record fades:** start/stop produces no discontinuity (max sample delta
  across fade boundaries); overdub level matches feedback.
- **Pitch accuracy:** 440 Hz sine in the buffer, PITCH +12 st → ≈880 Hz by
  zero-crossing count — in Digital AND Tape mode; Tape grain duration
  scales 1/ratio, Digital stays fixed (±1 window tolerance).
- **Chord distribution:** with a COLOR chord active, spawned grain ratios
  ⊆ the chord set, round-robin coverage; COLOR 0 → all grains on the root.
- **Scatter:** MOTION 0 → stereo width ≈ 0 and position spread within
  tolerance; MOTION 1 → spread bounded by the spec ranges; statistics from
  the seeded Rng are exact, not flaky.
- **STEP gating:** silence outside gate + release window; burst latches
  pitch at trigger time.
- **FLOW continuity:** standing cloud over several seconds — RMS never
  drops below threshold in any 50 ms window.
- **CHOKE hold:** `set_hold(true)` fades the cloud out click-free, release
  re-arms; the priority-window rules match the synth drone.
- **Play-while-recording:** grains during REC stay inside the current
  fill (never read ahead of the write head into stale memory); sound is
  present before REC stops.
- **Voice row:** FILT full left → silence for any lane (the FILT fade
  invariant, mirrored from the synth test); SUB share and DTUN spread
  statistically match their settings from the seeded Rng.
- **Factory autoload (VCV, play-test):** ENG flip with empty buffer →
  sound within one gesture; never overwrites existing content.
- **Empty/nullptr:** silence, no crash.
- **Determinism:** double render bit-identical.
- **Engine switch:** synth ↔ sampler click-free (M2 pattern extended).
- **Synth neutrality:** pinned baseline scenarios byte-identical with the
  M5 code merged (ENGINE never set to sampler).

## Acceptance demo

Scenario render: part A = synth phrase with COLOR, part B = sampler.
`input_wav` plays in, B records live, then a FLOW cloud that MOTION opens
from tight loop to fog, PITCH chord-locked to A's harmony; a switch to
STEP-chopped texture at the end. Output WAV + CSV (fill, grain count).

Rack play-test checklist (user, by ear): ENG flip → factory drone sounds
immediately; REC gesture on live input (texture emerges while recording);
resampling the own synth part via patch cable, menu load/save, patch
save/reopen with a recorded texture, MOTION sweep, COLOR shimmer, STEP
chop under CHOKE, voice-row sweep (ATK/DEC grain shape, FILT fade to
silence, SUB octave layer, DTUN spread).

## Roadmap placement

Milestone **M5 — texture deck**, after the chord layer (M4.10, done),
before M6 (firmware shell + hardware mapping). The M6 shell later maps:
ENGINE switch → ALT+PLAY cycle gesture, REC → the printed record gesture,
edit layer → sampler edit layer, WAV load/save → SD card slots through the
same `load_sample` entry point.

## Assumptions to verify during implementation

- `Buffer` desktop portability: confirmed at header level in the old spec;
  verify at link time when the copy first builds in the render host.
- The 42 s buffer next to the FX/echo/reverb memory in the M6 SDRAM map
  (original fit three 42 s buffers, two are safe — recheck when M6
  allocates).
- Grain spawn scheduling at control-tick granularity is fine for SIZE ≥
  20 ms (worst case ~2 ms quantization, hidden by spawn-offset jitter) —
  verify no audible combing at minimum SIZE.
- Rack patch-storage autosave: confirm `getPatchStorageDirectory()`
  behavior on the Rack 2.6 SDK across win/mac/linux CI targets.
- `set_gate` forwarding must not double-trigger with `trigger` in STEP
  (trigger latches, gate sustains — assert exactly one burst per fired
  step).
