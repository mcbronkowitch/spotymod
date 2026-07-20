# Spotykach Modulation-First Synth — Firmware Design

Alternative firmware for the Spotykach hardware: a two-part ambient instrument
where the modulation system is the primary interface — "modulation is the
instrument". Builds on Concept 1 of the faceplate study
(`2026-07-10-spotykach-firmware-faceplate-study-design.md`).

Fork: `github.com/mcbronkowitch/spotymod` (upstream `Synthux-Academy/Spotykach`),
local at `c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Rebuild the Spotykach firmware around two symmetric **parts**. Each part is a
**SuperModulator bank** (one performable macro surface driving five
independent, polyrhythmic modulation lanes) feeding a selectable sound engine
(granular sampler or polyphonic synth voice). A center
section (MORPH / COUPLE / DRIFT / SPOT) makes the interaction between the two
parts playable. CV and gate outputs extend the modulation system to the rest of
the rack.

No Spotykach device is currently available. The engine must therefore be fully
testable on the desktop: same C++ core compiled into an offline render host and
into the firmware shell.

## Decisions (from brainstorming)

- Sound source: **hybrid** — each part selects sampler or synth engine.
- Priority: the **modulation engine** is the core deliverable.
- Modulator model: **one SuperModulator per part** (no matrix, no coupled-chaos
  system as primary identity).
- Pad logic: **toggle + depth edit** (tap = target on/off, hold + knob = edit).
- Targets: **engine-specific** (each engine defines its own 5 targets).
- Center: **full** — MORPH, COUPLE, DRIFT (+ SPOT gesture).
- Synth engine: **4-voice polyphonic** with trigger-based voice allocation.
- Development: portable engine core + thin firmware shell (approach B).
- UX review (2026-07-10) incorporated: release-based pad gestures, fixed
  target slots across engines, RANGE minimum = off, sampler transport,
  LED visibility rules, fader layering with slew catch-up.
- Modulator bank (2026-07-10, second pass): five polyrhythmic lanes behind
  one macro surface — no single output driving all targets in unison.
- Standalone operation is a hard requirement (internal clock, no patch
  cables needed).
- Entropy sequencer (2026-07-12): looping S&H melody buffer per lane; bipolar
  ENTROPY (erode / loop / grow) replaces LOOP/EVOLVE; switch 2 remapped.
- **Melody engine rework (2026-07-14, supersedes the capture sequencer and the
  PROBABILITY knob; reworks the entropy negative half):** the bipolar control
  is renamed **MELODY / VARIATION** — RENEW ← LOOP → GROW (ERODE is gone). The
  PITCH lane gains a motivic **phrase generator** (five switchable principles),
  a per-step **gate** layer and a live **DENSITY** control; the other lanes keep
  the same knob as a plain LOOP / GROW / RENEW process. Capture/record/replay
  and PROBABILITY are removed. See
  `2026-07-14-spotykach-melody-engine-rework-design.md`; the sections below are
  updated to match.

## Architecture

One portable engine core, two hosts:

```
Spotykach/  (fork)
├── engine/                  NEW, platform-independent (no libDaisy includes)
│   ├── mod/                 SuperModulator, Drift, Coupling
│   ├── parts/               Part, engine interface, SynthVoice, SamplerEngine
│   ├── center/              Morph mix, center logic
│   └── instrument.h/.cpp    Instrument — the single public API
├── host/
│   └── render/              Desktop CLI: scenario JSON → WAV + CSV
├── src/                     existing firmware; new thin app shell added
│                            (original app.cpp kept as reference)
└── tests/                   desktop unit tests for engine/
```

**Boundary:** `Instrument` is the complete public API — `init(sample_rate)`,
normalized parameter setters (0..1 floats), pad events,
`process(in, out, size)`. No hardware type crosses into `engine/`. The same
code runs in the desktop renderer and on the Daisy.

**Reuse:** DaisySP is portable and used on both targets. The existing Deck/Vox
granular engine depends only on `Buffer` (not libDaisy) and is adapted behind
the engine interface. Clock/sync, storage, LED, pad and MIDI code from the
original firmware is inherited by the shell.

## SuperModulator (one per part): a bank of five lanes

One performable macro surface, five independent runs behind it. The macro
knobs (RATE, SHAPE, MELODY, SMOOTH, RANGE, DEPTH) drive **five lanes —
one per target** — each with its own phase and its own random stream, at a
fixed musical ratio of the master RATE. Shared character, independent motion:
the melody can rise while the filter falls. (A single output driving all
targets would move everything in unison — a tremolo, not an instrument.)

| Lane | Target slot                    | Rate ratio           |
|------|--------------------------------|----------------------|
| 1    | source/color (POSITION/TIMBRE) | ×2                   |
| 2    | size/focus (SIZE/FILTER)       | ×½                   |
| 3    | **PITCH**                      | **×1 (master lane)** |
| 4    | texture/motion (SHAPE/MOTION)  | ×¾                   |
| 5    | LEVEL                          | ×1½                  |

The PITCH lane leads: it defines "one cycle" (the phrase length), its gated
step events start synth voices, and it feeds the part's CV/gate outputs. Ratios
are fixed in v1 (no menus, no hidden parameters); editable ratios are a
later extension.

Per-lane signal path (the PITCH lane carries the gate/density layer; the other
four have no gate and every step fires):

```
[wavetable core] → [gate] → [step/flow] → [smooth] → [range] → lane output (bipolar -1..1)
     ↑                                                          └→ its target (per-target depth)
  rate×ratio, shape, variation           PITCH lane also → CV out, gate out, LED ring
```

Macro parameters (act on all lanes unless noted PITCH-only):

- **RATE** — speed. SYNC mode: quantized clock divisions (8 bars … 1/32),
  using the existing synclock/tempo infrastructure. FREE mode: continuous
  ~0.02 Hz to ~30 Hz.
- **SHAPE** — continuous morph through a waveform bank:
  sine → triangle → ramp → pulse → sample&hold random.
- **MELODY / VARIATION** (bipolar, continuous, centre-detent — the CTRL_POS
  pot) — 0 (LOOP): deterministic, every cycle identical. Positive (GROW): the
  existing pitches take small root-gravity walks and shape/phase/rate wander via
  the EVOLVE walk; the rhythm holds. Negative (RENEW): whole motifs (a Q&A pair,
  a cell lineage) are regenerated by the current principle — genuinely new,
  structured phrases, not eroded. Uniform sign across all five lanes; the PITCH
  lane runs the rich phrase generator, the others a plain contour-walk RENEW.
  (See `2026-07-14-spotykach-melody-engine-rework-design.md`.)
- **GATE / DENSITY** (PITCH lane) — the melody carries a per-step gate
  (note / rest); a gated step triggers a voice and drives the gate out, a rest
  is a real rest that holds pitch. **DENSITY** (a live PITCH-lane control, SEQ
  hold + CTRL_POS) thins/fills the gate pattern deterministically by metric
  weight — weak beats drop first — non-destructively, so it is exactly
  reversible. This replaces the old PROBABILITY dice as the part's trigger
  source: triggers are the PITCH lane's gated steps.
- **STEP / FLOW** (toggle) — FLOW: continuous output (no gates, no motif
  structure — a single shaped value). STEP: output quantized to clock
  subdivisions (sample & hold) — waves become sequences; at the S&H end of SHAPE
  the sequence is the generated, motivic melody buffer varied by MELODY — not a
  free random.
- **SMOOTH** — slew limiter on the output; turns steps into glides.
- **RANGE** — output scaling and polarity, monotonic: minimum = off, opening
  unipolar through the first half, widening to full bipolar at maximum.
  No off-point mid-travel (that would need a center detent the pot doesn't
  have).
- **DEPTH** — master intensity across all active targets (multiplies
  per-target depths).

All lanes keep running, even at depth 0, so LED ring and CV out never freeze
and fading in via DEPTH is phase-stable.

## Parts and engines

A part = SuperModulator + selectable engine + 5 target pads. Both parts are
identical, mirroring the hardware.

**Engine selection:** hold ALT + hold the part's PLAY pad for **≥ 1 s**. The
ring blink-counts down in the target engine's color; releasing early aborts.
A short ALT+PLAY tap is record-arm (see sampler transport), never the engine
switch — changing the meaning of all five pads must be deliberate. The active
engine is shown permanently on the part's CYCLE LED (orange = sampler /
tape-warm, teal = synth / cold); on switch the whole ring floods in the new
color. Persisted in settings.

**Fixed target slots:** the five pads keep the same function class in both
engines — switching engines never moves a function to another pad:

| Slot  | Function class     | Sampler  | Synth  |
|-------|--------------------|----------|--------|
| Pad 1 | sound source/color | POSITION | TIMBRE |
| Pad 2 | size/focus         | SIZE     | FILTER |
| Pad 3 | **PITCH (always)** | PITCH    | PITCH  |
| Pad 4 | texture/motion     | SHAPE    | MOTION |
| Pad 5 | **LEVEL (always)** | LEVEL    | LEVEL  |

**Engine 1 — Sampler** (adapter around existing Deck/Vox granular engine).
Targets on the 5 pads:

| Pad | Target   | Acts on                              |
|-----|----------|--------------------------------------|
| 1   | POSITION | playhead/start in buffer             |
| 2   | SIZE     | slice length                         |
| 3   | PITCH    | speed/pitch (tape or digital mode)   |
| 4   | SHAPE    | grain window shape/envelope          |
| 5   | LEVEL    | part volume                          |

**Sampler transport:** record-arm is toggled with **ALT + PLAY tap**
(non-destructive — nothing is erased until recording actually starts); the
armed pad blinks. The next PLAY tap starts recording from the inputs, another
stops it; the ring shows buffer fill. With an empty buffer a plain PLAY tap
also arms (there is no meaningful target to toggle yet). Loading WAVs from SD
uses the inherited storage mechanism. ALT + PLAY **held ≥ 1 s** remains the
engine switch — tap and hold are disjoint by the same release/hold rules as
all pad gestures.

**Engine 2 — Synth voice** (new, DaisySP building blocks). 4-voice polyphonic:

- Every **trigger** starts a voice (round-robin, oldest stolen). Trigger
  sources: the part's gate in, and the PITCH lane's gated step events.
- Pitch is **latched per voice at trigger time**: V/Oct CV + current modulator
  value on the PITCH target, quantized to semitones/scale.
- Each voice: 2 detuned oscillators + sub, wavetable morph, lowpass filter,
  own attack/decay envelope. Long decays overlap into pads.
- In FLOW mode without triggers the part behaves as a drone (one held voice,
  continuously modulated).

Targets:

| Pad | Target | Acts on                          |
|-----|--------|----------------------------------|
| 1   | TIMBRE | oscillator morph/detune          |
| 2   | FILTER | cutoff                           |
| 3   | PITCH  | pitch (quantized to scale)       |
| 4   | MOTION | stereo spread/chorus movement    |
| 5   | LEVEL  | part volume                      |

**Pad logic (toggle + depth edit):**

The pads are capacitive and the original firmware fires on touch; to avoid
false triggers, all pad actions here fire **on release**, with explicit abort
rules:

- **Tap** (released within ~300 ms, no knob moved) = target on/off.
- **Hold** (> ~300 ms, or any big knob moved while holding) = edit mode; the
  toggle is suppressed on release. While held, the part's two big knobs
  temporarily become **BASE** (target base value) and **DEPTH** (modulation
  depth); on release they revert to RATE/DEPTH via soft takeover (MValue).
- **Hold-to-inspect:** the moment a pad is held, the ring shows that target's
  BASE and DEPTH — before any knob movement. Inspecting a value never
  requires changing it (5 targets × 2 parts × 2 engines = 20 stored BASE
  values; this is how they stay visible).
- **Two pads held on the same side:** the first wins; later pads are ignored
  until it is released.
- After an edit, RATE/DEPTH are in their pickup dead zone; while turning
  inside it, the ring shows the pickup point as a bright segment so the knob
  never feels broken.

**Knob mapping per side** (7 existing pots):

| Hardware pot   | Function            |
|----------------|---------------------|
| CTRL_MODFREQ_x | RATE                |
| CTRL_MOD_AMT_x | DEPTH               |
| CTRL_SOS_x     | SHAPE               |
| CTRL_POS_x     | MELODY (+ DENSITY on SEQ hold) |
| CTRL_ENV_x     | SMOOTH              |
| CTRL_SIZE_x    | RANGE               |
| CTRL_PITCH_x   | TUNE (engine, root) |

The printed faceplate labels conflict with several new functions
(POS → MELODY, SOS → SHAPE, SIZE → RANGE, PLAY → target toggle). This
is an inherent cost of an alternative firmware; the answer is consistent
internal logic plus a printed **faceplate overlay** — which ties directly
into the faceplate study.

## Melody controls (per part, PITCH lane)

The PITCH lane's phrase generator is performed from the CTRL_POS pot and the
SEQ pad (both freed by removing capture and PROBABILITY). Full behaviour in
`2026-07-14-spotykach-melody-engine-rework-design.md`; the surface:

- **MELODY** = CTRL_POS pot, bipolar with a centre detent: RENEW ← LOOP → GROW.
  Hold to move the phrase, release to freeze wherever it landed (process, not
  state — releasing keeps the current phrase, it is not an undo).
- **Cycle principle** = **SEQ-pad tap** (fires on release, short). Steps through
  the five phrase principles — Two motifs (default), One motif + variation,
  Hierarchical, Question & answer, Ostinato; the ring shows a brief principle
  overlay. The SEQ label the firmware finally makes true.
- **DENSITY** = **SEQ-pad hold + CTRL_POS**, continuous, soft-takeover; the ring
  shows density. Thins/fills the gate pattern by metric weight, non-destructive.
  Soft-takeover cuts both ways: on SEQ release, MELODY re-engages only once the
  pot crosses its pre-hold value — no variation jump after a density edit.
- **New phrase now** = **ALT + SEQ-pad**: regenerate the whole phrase (fresh
  arrangement + all motifs) with the current principle, then loop it — the
  "audition a new melody, then leave the knob at LOOP" gesture.

Tap and hold are disjoint by the same release/hold rules as all pad gestures.
The generator is a bank feature: on a sampler part the same MELODY/DENSITY/
principle controls shape the PITCH-target pattern and its triggers.

## Center section

Hardware available in the center: crossfade fader (+ CV in), SPOT and ALT
pads, 3-position mode switch, clock jack, physical TAP button.

- **MORPH** = fader directly (+ CV): equal-power blend A ↔ B.
- **COUPLE** = hold ALT + move fader: mutual influence between the two
  banks — A pulls B's master phase and rate and vice versa. Low = loose
  relationship, high = they lock in.
- **DRIFT** = hold SPOT (past the hold threshold) + move fader: global
  "weather" — a very slow random walk shifting both banks' master rate/shape
  and both engines' tuning.
- **SPOT tap** = the eponymous stumble: a jolt — every lane of both banks
  gets its own random phase/shape kick, decorrelating them further. DRIFT is
  slow weather, SPOT is lightning.

**Fader layering rules** (three values on one fader — the critical part):

- While a modifier is held, both LED rings display the layered value (COUPLE
  or DRIFT) as a bar, with the pickup point as a bright segment.
- Back on MORPH, the fader position may disagree with the actual mix. MORPH
  neither jumps nor goes dead: on the first fader move it **catches up by
  slewing** (~0.5–1 s glide toward the fader position), then tracks 1:1.
- **CV_CROSSFADE always acts on MORPH only**, regardless of held modifiers.
- ALT and SPOT held simultaneously: whichever was pressed first wins; the
  second is ignored until release.

**SPOT gesture rules:** the jolt fires **on release**, only if the touch was
short (< ~250 ms) and the fader did not move meanwhile — reaching for a DRIFT
edit or brushing the pad while riding the fader never fires it. DRIFT edit
becomes active after the hold threshold, confirmed by the rings switching to
the DRIFT display.

**TAP button:** short tap = tap-tempo (as in the original) when no external
clock is present; **hold ≥ 1 s = settle** — DRIFT amount to zero and all
EVOLVE/variation walks re-centered. The panic gesture for "why is everything
slowly going wrong".

The 3-position mode switch keeps the existing stereo routing
(double-mono / stereo / generative stereo). Serial routing (A into B) is a
later extension, out of scope for v1.

## Panel switches

Verified inventory (UX review): three physical 3-position (ON-OFF-ON)
switches per side, plus the center mode switch and the TAP button. Binary
functions get an explicit, useful middle position instead of undefined
behavior:

| Switch | Left | Middle            | Right         |
|--------|------|-------------------|---------------|
| 1      | SYNC | SYNC (triplets)   | FREE          |
| 2      | free (reserve) | free (reserve) | free (reserve) |
| 3      | STEP | STEP + fixed slew | FLOW          |

Switch 2 is freed: the LOOP / GROW / RENEW axis it used to carry (ERODE / LOOP /
GROW) now lives on the continuous MELODY knob.

## LED feedback and state visibility

Rule: **every source of inaudible, creeping change gets its own
always-visible indicator.** Attribution — "why is the sound changing right
now?" — is a feature.

- **Ring (32 LEDs per side):** shows the **PITCH lane** (the musically
  leading information) **post-range, post-gate, post-density** — what actually
  reaches the target, not the raw wave. On a rest (gate off, or a step masked
  out by DENSITY) the ring dims to signal the held/rest state. A **SEQ tap**
  briefly overlays the active principle. The ring doubles as value display for
  hold-to-inspect, layered fader values, DENSITY edit and pickup points.
- **Target pads:** LED on = target active; brightness follows **its own
  lane's** modulation actually applied (|lane × master depth × target
  depth|) — a dead chain is visibly dead. Pad 5 (LEVEL) has **no LED in hardware** (verified); its
  state is shown on a fixed edge segment of the ring instead.
- **CYCLE LED per side:** permanent engine color (orange = sampler,
  teal = synth).
- **SPOT pad LED:** glows proportional to DRIFT amount and pulses with the
  random walk ("weather lightning").
- **FADER LEDs A/B:** static glow proportional to COUPLE strength.
- **ALT LEDs:** lit while an ALT layer is active.

## Rack integration

- **CV outs (DAC A/B):** each part's PITCH lane (quantized pitch as CV) —
  the generated melody is directly patchable.
- **Gate outs A/B:** the PITCH lane's gated step events as gates. The CV+gate
  pair makes the instrument a standalone melody sequencer for the rack.
- **Clock in** + per-part SYNC/FREE.
- **V/Oct ins** per part → synth engine pitch.
- **CV ins:** per part, CV_SIZE_POS_x → RATE, CV_MIX_x → DEPTH;
  CV_CROSSFADE → MORPH. CV always acts on the base function — never on a
  layered or edit value.

## Standalone operation

The instrument is complete without a single patch cable (hard requirement):

- Internal clock with tap-tempo (TAP button) and a default BPM; external
  clock overrides when present.
- TUNE knobs set each part's root; scale quantization is internal.
- Triggers come from the PITCH lanes' gated step events (or gate ins when
  patched).
- V/Oct, clock, gates and CV ins are optional additions, never requirements.

## Desktop render host

- CLI tool, builds with CMake on Windows, uses the identical `engine/` code
  plus DaisySP.
- Input: a **scenario file** (JSON): initial state of all parameters/pads plus
  a timeline of changes ("at 10s: variation to −0.4", "at 20s: SPOT tap").
- Output: `out.wav` (stereo audio) + `mods.csv` (modulator outputs, trigger
  events, voice states over time — plots from this double as portfolio
  material).
- The sampler part loads WAV files from disk instead of SD card.

## Testing

TDD on the modulation logic; unit tests run on the desktop against `engine/`:

- rate accuracy in sync and free mode
- step quantization
- gate/rest and DENSITY behaviour (deterministic, metric-ordered, reversible)
- slew behavior
- voice allocation (round-robin, stealing)
- couple symmetry
- lane decorrelation (rate ratios, independent random streams per lane)
- melody determinism (variation 0 loops the phrase exactly — pitch and gate —
  while live lanes keep moving); phrase generator produces motivic repetition,
  not note salad

Sound character (do GROW/RENEW feel organic and musical?) is verified by ear
via renders.

## Technical constraints

- **No heap, no allocation in the audio path** — everything static, following
  the existing firmware (NOCOPY idiom).
- **CPU budget:** block 96 samples @ 48 kHz. Enable the existing `METER`
  early; target < 70% load worst case (both parts synth, 8 voices, FX).
- **Parameter smoothing:** all knob→engine paths through the existing OnePole
  smoothers.
- **Error behavior:** missing/broken SD card → sampler part runs empty
  (no crash), LED signals it; settings read failure → defaults.
- **Firmware shell:** new app entry next to the old one (`app.cpp` kept as
  reference), switchable via Makefile target — the original firmware remains
  buildable at all times.

## Assumptions to verify during implementation

- **Toggle inventory: verified** (UX review, 2026-07-10) — three 3-position
  switches per side, center mode switch, physical TAP button; mapped in
  "Panel switches" above.
- Deck/Vox portability: confirmed at header level (depends on `Buffer` only);
  verify at link time when the render host first builds.
- DAC CV out range/scaling: reuse the existing DAC callback conventions.
- Gesture thresholds (~250/300 ms, ≥ 1 s) are starting values — tune on
  hardware once a device is available.
- Physical position of the CYCLE/GATE LEDs relative to the pads: confirm
  against the panel when hardware arrives (affects which LED best shows the
  pad-5 LEVEL state).

## Build order

1. Modulator bank (5 lanes) + unit tests + render host (make it audible
   first)
2. Polyphonic synth voice
3. Phrase generator + gate/DENSITY layer (melody engine rework)
4. Center (COUPLE / DRIFT / MORPH / SPOT)
5. Sampler adapter
6. Firmware shell + hardware mapping

## Acceptance criteria (v1)

- `engine/` compiles and runs identically on desktop and (later) on Daisy;
  no libDaisy include inside `engine/`.
- Render host produces WAV + CSV from a scenario file; a demo scenario with
  STEP mode + PITCH target + a MELODY sweep produces an evolving, quantized,
  motivic melodic texture with overlapping voices.
- Melody demo: at variation 0 the PITCH phrase (pitch + gate) repeats
  identically while the other lanes keep moving; sweeping MELODY grows/renews
  it and DENSITY thins/fills the rhythm (visible in mods.csv).
- Unit tests for the modulator behaviors above pass, including the gesture
  state machine (tap/hold thresholds, release aborts, modifier precedence).
- Firmware target builds (`make`) even without hardware to flash.
- Original firmware remains buildable from the same fork.
