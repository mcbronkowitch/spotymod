# Spotykach Firmware Shell — M6 Design

The firmware shell for the modulation-first firmware: a platform-independent
`shell/` layer that turns panel input into `Instrument` calls and LED frames,
plus a thin Daisy adapter (`app_mod.cpp`) and the hardware bring-up (SDRAM
map, timers, DAC, storage). M6 also resolves the **complete gesture map** —
including the UI debts every earlier milestone deferred here — and settles a
pad-assignment conflict that had accumulated across the specs.

Extends the main design (`2026-07-10-spotykach-modulation-first-synth-design.md`);
milestone **M6**, last in the build order, implemented after M3–M5 whose
engine APIs this spec takes as given (their specs are final).

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Everything between the hardware and the engine: gestures, layers, knob
takeover, LED feedback, switches, clock/MIDI plumbing, persistence, CV/gate
I/O — designed and testable **without a device**. The gesture/UI logic is
pure logic in `shell/` (desktop unit tests + scenario host); only a short
on-hardware checklist at the end genuinely needs the instrument.

## Decisions (from brainstorming, 2026-07-12)

- **Portable shell core (approach A).** All UI logic lives in a new
  platform-independent `shell/` layer (no libDaisy, no heap), consumed by
  two adapters: the Daisy firmware and the desktop scenario host. Future
  custom hardware = a third thin adapter; the tested logic is untouched.
  The core is designed concretely for the Spotykach panel — only the
  *source* of events is abstracted, not the interaction logic (no generic
  UI framework).
- **Full-instrument scope.** The spec assumes the M3–M5 engine APIs as
  specified. No staged bring-up subset: the gesture map is too interwoven
  to define a useful temporary subset (and there is no device to flash
  anyway).
- **Testing: unit tests + scenario panel host (approach A).** Gesture state
  machine covered by doctest; the render host gains panel-event scenario
  actions that drive the full path panel event → shell → Instrument, with
  parameter-call and LED logs as assert targets.
- **MIDI minimal:** MIDI clock in as a third clock source, note-in →
  trigger + pitch per part (channel 1 → A, channel 2 → B). No CC map, no
  MIDI transport, no clock out in v1.
- **Persistence: everything hand-set persists.** All values set through
  hold+knob edit gestures (no physical home on the panel) survive a power
  cycle, in one versioned QSPI block. Deliberately volatile: capture loops
  (M3), COUPLE/DRIFT/MORPH (performance state, M4 boot defaults).
- **Pad principle: targets first.** The plain layer of the five per-side
  pads belongs to the modulation targets (the fork's identity); all
  functions move behind ALT — where they match the printed faceplate
  labels. See "The pad conflict and its resolution".

*Amended 2026-07-12 after a three-lens cross-spec review (UX coherence /
embedded feasibility / promise accounting): scales interface re-homed to
the scales-spec model, layer latching, engine deltas (V/Oct, MIDI note,
soft-clip, fast_sin), CV-in routing, sampler content gestures +
monitoring, factory state + reset, main-loop pad polling, budget checks,
middle-position constants, gate protocol.*

## The pad conflict and its resolution

The hardware has **five touch pads per side** (MPR121, 12 channels:
PLAY, REV, GRIT, FLUX, SEQ per side + SPOT + ALT in the center). The master
spec needs five *target* pads per side — necessarily the same five pads —
but M1.6 and M2 later claimed plain-layer gestures on three of them
(FLUX/GRIT tap = FX on/off, PLAY tap = manual trigger). A tap cannot both
toggle a target and trigger a voice; the earlier specs left the final call
to M6 ("final gesture design is an M6 decision", "the engine API is
agnostic").

**Resolution — targets first:** plain tap/hold on all five pads = engine
targets, exactly as the master spec defines. Every function gesture moves
behind ALT, where it lands on the pad's *printed* label. The product
photo confirms the fit: the faceplate prints "record" bracketing
PLAY-towards-ALT, and "⦀⦀⦀ + alt" next to the SEQ pads — the panel already
documents ALT+PLAY = record and ALT+SEQ combos.

Superseded gestures from earlier specs (all engine APIs unchanged):

| Earlier spec said | M6 replaces with |
|---|---|
| M2: PLAY tap = manual trigger | ALT+PLAY tap (synth part) |
| M2: PLAY hold = VOICE edit layer | ALT+REV hold (synth part) |
| M1.6: FLUX tap/hold = on-off / FLUX edit layer | ALT+FLUX tap/hold |
| M1.6: GRIT tap/hold = on-off / GRIT edit layer | ALT+GRIT tap/hold |
| M1.6: ALT+GRIT tap = Drive↔Reduce | two-zone knob inside the GRIT edit layer |
| M1.6: ALT+FLUX hold = separate reverb layer | reverb globals folded into the FLUX edit layer |
| Master: empty-buffer plain PLAY tap also arms | arming is always ALT+PLAY (plain PLAY is always target 1) |
| Scales: ALT + PITCH-pad tap = cycle the part's quantize mode | SHAPE knob, 3 zones, inside the PITCH pad's edit layer |
| Scales: ALT hold alone = passive quantize inspection on the ring | folded into PITCH-pad hold-to-inspect (ALT-hold shows the COUPLE bar, per M4) |
| M1.6/M2: FX and engine edit layers active only while the pad is held | ALT-entered edit layers **latch**; plain ALT tap exits (see "Layer latching") |

The scales spec's *model* is untouched: **one global scale + a per-part
quantize mode** (SCALE / CHROM / FREE), and global scale selection stays
the scales spec's **relative stepping on ALT + TUNE** (deliberately not an
absolute knob position — that spec's two-owners-one-knob argument stands).
Only the mode gesture and the inspection display move, as above.

## Pad map

Physical pad order = target slot (MPR channel order); SEQ is the only pad
without an LED, matching the master spec's "pad 5 (LEVEL) has no LED":

| Physical pad | Target slot (plain) | Printed function (ALT layer) |
|---|---|---|
| PLAY | 1 — POSITION / TIMBRE | trigger / transport / engine switch |
| REV  | 2 — SIZE / FILTER | reverse (sampler) / engine edit layer |
| GRIT | 3 — **PITCH** | GRIT on-off + edit |
| FLUX | 4 — SHAPE / MOTION | FLUX on-off + edit |
| SEQ  | 5 — LEVEL | capture |

**Plain layer** (unchanged master-spec grammar): tap (released < ~300 ms,
no knob moved) = target on/off; hold (> ~300 ms or knob moved) = edit —
RATE knob → BASE, DEPTH knob → DEPTH, hold-to-inspect on the ring
immediately, toggle suppressed on release, first held wins.

**Scales interface** (the model from the scales spec, re-homed):

- **Global scale:** ALT + turn TUNE = relative stepping through the six
  scales (the scales spec's gesture, kept). Active only while no edit
  layer is latched; the ring shows the scale index transiently while
  stepping. Changing the global scale retunes both parts — that is the
  point of a global scale.
- **Per-part quantize mode:** in the **PITCH pad's edit layer**, the
  SHAPE knob selects SCALE / CHROM / FREE — 3 zones with hysteresis, via
  soft takeover like every layer knob. The PITCH-pad hold-to-inspect
  display includes scale index and mode.
- Root stays on TUNE in the plain layer (see "Knobs" for when TUNE is
  borrowed).

**ALT layer** (pads do what the faceplate prints):

| Gesture | Synth part | Sampler part |
|---|---|---|
| ALT+PLAY tap | manual trigger at current quantized pitch | transport cycle: arm → record → stop (contextual) |
| ALT+PLAY hold ≥ 1 s | engine switch: ring countdown in target color, early release aborts | same |
| ALT+REV tap | — (reserved, no function in v1, LED off) | reverse on/off |
| ALT+REV hold | VOICE edit layer (knob map from the M2 spec; only the entry gesture moves here) | sampler edit layer: win_size, overdub feedback, speed mode Tape/Digital (two-zone knob with hysteresis) |
| ALT+GRIT tap / hold | GRIT on-off / GRIT edit layer: RATE → INTENSITY base, DEPTH → GRIT MIX, SHAPE → Drive↔Reduce (two zones, hysteresis; LED warm = Drive, cold = Reduce) | same |
| ALT+FLUX tap / hold | FLUX on-off / FLUX edit layer: RATE → TIME, DEPTH → FEEDBACK, SHAPE → FLUX MIX, SMOOTH → REVERB SEND base, PROBABILITY → REVERB SIZE, RANGE → REVERB TONE, TUNE → SHIMMER | same |
| ALT+SEQ tap / hold | replay toggle (first tap with no stored loop = capture + replay) / capture fresh — per the M3 spec's suggested mapping | same (capture is engine-agnostic) |

**Layer latching:** every ALT-entered edit layer (FLUX, GRIT, engine
edit) **latches**: ALT+pad hold enters it, then both fingers can lift and
the layer stays active. Exit = a plain ALT **tap** (otherwise unassigned),
or entering another ALT layer. While latched, the ALT LED blinks and the
pad LEDs show the layer's meaning — the mode is unmissable. Rationale:
without the latch, editing inside a layer is a three-contact gesture
(center ALT + side pad + side knob); latched, everything inside a layer
is the standard two-contact pad+knob grammar.

Inside a latched **FX layer** (FLUX or GRIT), the five pads stand for the
five FX targets: tap = toggle, hold + DEPTH knob = mod depth (slot
mapping from the FX spec: 1 GRIT INT, 2 FLUX TIME, 3 FX MIX, 4 REV SEND,
5 FLUX FB); the layer knobs above set the bases. ALT+pad combos keep
their normal function-layer meaning even while latched. (Note: FX slot
names do not land on their eponymous pads — GRIT INTENSITY sits on the
PLAY pad. This follows the FX spec's fixed lane-index = slot mapping,
which is musical/DSP-behavioral and stays; the faceplate overlay labels
it.)

Inside the latched **sampler edit layer** (ALT+REV on a sampler part),
the pads carry the sampler's content management: **PLAY tap = clear the
buffer** (ring floods dark as confirmation), **SEQ tap = save the buffer
to the part's SD slot** (`A.wav` / `B.wav` in the card root). At boot the
shell autoloads each part's slot file if present, through the inherited
storage path. Mid-session loading beyond the slot files is out of scope
for v1. Inside the synth part's latched VOICE edit layer the pads are
reserved (no function in v1).

**Recording rules:** while a sampler part is armed or recording, the dry
input is monitored through the part chain (see the amended M5 spec —
automatic, no toggle). Starting the engine-switch countdown (ALT+PLAY
hold) during recording first stops the recording (content kept), then
counts down.

**Center** (wiring only; behavior per master/M4 specs): fader plain =
MORPH with catch-up slew (~0.5–1 s) after any layered use; ALT held +
fader = COUPLE; SPOT held past ~250 ms + fader = DRIFT; SPOT tap
(released < ~250 ms, fader unmoved, fires on release) = the kick;
ALT and SPOT simultaneously: first pressed wins; CV_CROSSFADE always acts
on MORPH; TAP short = tap tempo (when no external clock), TAP hold ≥ 1 s
= SETTLE. ALT is a global shift: pads and fader interpret it
independently (holding ALT allows both a pad function and COUPLE editing
in one hold — harmless and deliberate).

**Panic:** SETTLE additionally acts shell-side as the audio safety
gesture: any FLUX FEEDBACK base above 0.95 is pulled down to 0.95
(ordinary setter calls — no engine change). Together with the master
soft-clip (see "Engine deltas") this bounds the worst case of
lane-modulated self-oscillation while the player's hands are elsewhere.

**Switches** (three 3-position switches per side, master-spec functions
bound to physical switches; final printing is the faceplate overlay's
job):

| Physical switch (printed) | Function |
|---|---|
| reel / slice / drift | SYNC / SYNC triplets / FREE |
| small toggle at POS | LOOP / EVOLVE subtle / EVOLVE strong |
| waveform icons (at the blob) | STEP / STEP + fixed slew / FLOW |

Middle-position constants (starting values, tune by ear): EVOLVE subtle
= `set_evolve(0.3)`, strong = `set_evolve(1.0)`; "STEP + fixed slew" =
STEP mode with SMOOTH **floored** at 0.35 (the knob can push higher, the
switch guarantees the floor).

Center mode switch keeps the original stereo routings. The TAP button and
mode switch are read through the inherited shift-register path
(`sr_165`).

**Knobs** (plain layer, per master spec): cycle → RATE, glow → DEPTH,
mix → SHAPE, pos → PROBABILITY, env → SMOOTH, size → RANGE, big knob →
TUNE. The 32-LED ring physically encircles the TUNE knob. Naming rule:
the spec and code name knobs by *function*, never by size — the master
spec's "two big knobs" phrase meant RATE/DEPTH, which are physically the
two small blob knobs. The analog input-gain trimmers next to the audio-in
jacks ("!" marks) are hardware-only and invisible to firmware — not
available as controls.

**TUNE liveness:** TUNE (root) is live in the plain layer and during
ALT-scale stepping. Inside a latched FX layer it is borrowed like any
other knob (FLUX layer: TUNE → SHIMMER) under the same soft-takeover
rules — on layer exit the root does not jump, TUNE is in its pickup dead
zone until collected, exactly like RATE/DEPTH after any edit. Root-tuning
mid-FX-edit is not a workflow worth a knob conflict.

**Terminology note:** the sampler's pad-4 target is documented and
overlay-labeled **WINDOW** (grain window shape) from here on — "SHAPE"
already names the macro waveform knob, and the two are simultaneously
live on a sampler part. Engine target ids are unchanged.

**MIDI (minimal):** clock in (24 ppq) through the inherited synclock as a
third clock source (priority: external jack clock > MIDI clock >
internal/tap); note-in channel 1 → part A, channel 2 → part B — note
number → `trigger_note` (see "Engine deltas"), quantized like every other
pitch. Inherited USB MIDI transport.

## CV inputs

The master spec's CV-in promises, wired here (all per part unless noted):

| CV in | Acts on | Law |
|---|---|---|
| CV_V_OCT_x | synth pitch | `set_pitch_cv_offset` in semitones, summed **pre-quantizer** at voice-latch time (scales-spec rule) |
| CV_SIZE_POS_x | RATE (base) | additive bipolar offset: `effective = clamp01(knob + cv)` |
| CV_MIX_x | DEPTH (base) | same additive law |
| CV_CROSSFADE | MORPH | always MORPH, regardless of held modifiers (master rule) |

CV always acts on the **base function**, never on a layered or edit
value; it is an offset applied after soft takeover, so pickup logic never
sees it. Gate in → part trigger, unchanged.

## Engine deltas

M6 is shell-first, but four small `engine/` changes are unavoidable and
are made explicit here (everything else consumes M1–M5 APIs as-is):

1. `set_pitch_cv_offset(part, float semitones)` — V/Oct in, summed
   beside TUNE pre-quantizer; the rack CV out stays cleanly quantized.
2. `trigger_note(part, float semitones)` — MIDI note-in; an ordinary
   trigger whose pitch offset is latched for that voice.
3. **Master soft-clip** at the `Instrument` mix stage (after morph sum +
   reverb): a gentle `SoftClip`, bit-transparent below ~-1 dBFS. The
   safety net under modulated FLUX FEEDBACK > 1 and shimmer regeneration.
   **[Superseded by M4.6, 2026-07-13]:** delivered early as the
   stereo-linked master limiter (`engine/fx/limiter.h`: gain riding +
   piecewise ceiling, exactly bit-transparent below the −1 dBFS knee at
   drive 0) plus `set_master_drive` (pre-gain 1–4×). M6 only wires the
   gestures — suggested homes per the dynamics spec: GRIT layer
   SMOOTH → COMP per side, FLUX-layer TUNE (ex-shimmer) → MASTER DRIVE.
4. **`fast_sin` swap** in the mod-lane path: `shape_value`'s per-sample
   `std::sin` (10 lanes) and `part_fx`'s per-sample equal-power sine move
   to the existing tested `fast_sin` — the M2 spec explicitly deferred
   this to M6 (~8–12 % of the cycle budget at stake). Rendered bits
   change: pinned scenario WAVs re-render once.

## Architecture

```
Spotykach/
├── engine/            M1–M5 as specified + the four "Engine deltas"
├── shell/             NEW — all UI logic, portable (namespace spky::shell,
│   │                  no libDaisy, no heap, control-rate only)
│   ├── panel.h        data contracts: PanelIn (12 pad bits, 15 pots incl.
│   │                  fader, 6 side switches + mode switch, TAP button,
│   │                  CV ins, gate/clock edges, MIDI events) + LedFrame
│   │                  (RGB array, full LED_LAST length)
│   ├── gesture.h/.cpp per-pad gesture state machine: tap/hold thresholds,
│   │                  release aborts, knob-moved-cancels-tap, first held
│   │                  wins, ALT precedence, engine-switch countdown
│   ├── layers.h/.cpp  layer manager: plain/ALT/edit/FX layers — who owns
│   │                  pads and knobs right now (the gesture-map resolution)
│   ├── takeover.h/.cpp soft takeover per knob × layer (port of
│   │                  src/ui/mvalue — pure logic, copied so shell/ has no
│   │                  src/ dependency), incl. pickup dead-zone state
│   ├── leds.h/.cpp    LED renderer: Instrument getters + layer state →
│   │                  LedFrame; central color palette table
│   └── shell.h/.cpp   facade: tick(PanelIn, dt) → Instrument setters;
│                      render_leds() → LedFrame; settings_dirty() flag;
│                      settings serialize/deserialize (POD block)
├── host/render/       extended: panel-event scenario actions → Shell;
│                      params.csv + leds.csv outputs
├── src/               inherited hardware classes, unchanged
├── app_mod.cpp        NEW firmware entry: Hardware ↔ Shell ↔ Instrument,
│                      SDRAM map, timers, DAC, storage wiring
└── app.cpp            original kept; entry selected via Makefile flag
                       (APP=mod | APP=stock), original always buildable
```

**Core contract:** `Shell` sees only `PanelIn` in and `LedFrame` +
`Instrument` calls out. The Daisy adapter and the scenario host feed the
same shell.

**Data flow on the Daisy** (the original firmware's proven pattern —
including its rule that **blocking I2C/SPI stays in the main loop**;
`ProcessPads()` / `ProcessDigitalControls()` are documented blocking
calls and the stock firmware never runs them in an ISR):

- **Main loop:** poll MPR121 pads and the `sr_165` shift register
  (TAP button, mode switch) — both blocking; publish them into a
  lock-free `PanelIn` snapshot (single writer, sequence counter);
  storage processing (debounced settings writes), bootloader check
  (BOOT held 3 s), debug log.
- **Audio callback** (48 kHz, block 96): `ProcessAnalogControls` (ADC,
  non-blocking) → `Shell::tick(latest PanelIn snapshot, dt)` once per
  block → normalized `Instrument` setters → `Instrument::process()`.
  Every 8th block (~62 Hz) the shell also composes the `LedFrame` into a
  double buffer — pure computation, no I/O. `METER` compiled in from day
  one.
- **Timer T5 (250 Hz)**: gate-in edges, tempo prepare; every 4th tick
  push the latest complete `LedFrame` to the WS2812 chain (DMA), as in
  the original.
- **DAC callback:** CV/gate outs from `Instrument` getters. Protocol:
  CV = the quantized-pitch float snapshot (single word, race-benign);
  gate = the part's existing 5 ms trigger counter read as a bool.
  Trigger→gate jitter is bounded by one audio block (2 ms) — accepted
  for the "standalone melody sequencer" promise. `__USAT` scaling as in
  the original.

**SDRAM map** (in `app_mod.cpp`, `SDRAMBuffer` pattern): 2 × 42 s stereo
sampler buffers (~32.3 MB) + 4 FX echo buffers (3.7 MB) + `AmbientReverb`
(~0.5 MB) ≈ 36.5 MB of 64 MB — comfortable, as the M5 spec assumed.

**CPU and flash budget (eyes open):** the `AmbientReverb` lives in SDRAM
by necessity (`.bss` is 256 KB) — ReverbSc's scattered delay-line
accesses will mostly miss cache there, so the "≈10 % + 10 % shimmer"
estimates from M1.6/M2 may realistically land at 15–25 % combined. Two
consequences: (1) the **first** on-hardware measurement is METER with
reverb + shimmer alone, not the full patch (see checklist); (2) the
named fallback lever is **processing the reverb at half rate** (24 kHz
with up/down sampling) — decided by measurement, not folklore. Flash/RAM:
the new build (engine + shell + DaisySP FX + copied sampler core) must
fit the same 256 KB SRAM_EXEC text region as the stock firmware;
`arm-none-eabi-size` against the linker regions is an acceptance check
at build time — no device needed.

## LED feedback

`shell/leds` is a pure function of (Instrument getters, layer state,
gesture state) → full-frame `LedFrame` at ~62 Hz. Ring content follows a
strict **priority chain** — exactly one owner per frame per side:

1. hold-to-inspect (BASE/DEPTH bars + pickup segment) while a pad is
   held; for the held PITCH pad the display includes scale index + mode
2. layered fader value (COUPLE/DRIFT bar + pickup) while ALT/SPOT held;
   while ALT+TUNE scale stepping, the scale index shows transiently
   instead
3. engine-switch countdown blink (target engine color)
4. record state (sampler): armed = PLAY-pad blink + dim ring pulse;
   recording = buffer-fill arc growing around the ring (master-spec
   promise)
5. capture replay: static step pattern + rotating playhead (M3)
6. default: PITCH lane post-range, post-probability; dims while
   probability holds the last value; a fixed edge segment shows LEVEL
   (replacing the missing SEQ LED)

Pad LEDs: on = target active, brightness = |lane × master depth × target
depth| (a dead chain is visibly dead); in an FX layer they show the FX
targets instead. CYCLE LED = engine color (orange = sampler, teal =
synth; the square/round windows in the panel blobs). GRIT LED warm =
Drive, cold = Reduce. SPOT LED glows with DRIFT amount and pulses with
the weather walk. FADER LEDs = COUPLE strength. ALT LEDs lit while ALT is
held, **blinking while an edit layer is latched**. The clock-in LED
flashes the internal beat whenever no external clock is patched — tempo
is always visible and tap-tempo confirmable. Gate-in and mode LEDs behave
as expected. All colors live in one central palette table in `shell/`.

## Persistence

One new versioned settings block (own slug `"mod"`, `PersistentStorage`
in QSPI) **next to** the inherited `"cal"` block — calibration data and
the original calibrator remain untouched and usable.

Persisted: engine selection per part; target active flags, the 2 parts ×
2 engines × 5 BASE and DEPTH values; **global scale + per-part quantize
mode** (scales-spec model); internal tempo (tap-set BPM); full FX state
(FLUX/GRIT on-off, grit mode, static mixes, FX target
flags/bases/depths, reverb globals); voice edit params; sampler edit
params (win_size, feedback, reverse, speed mode). Deliberately volatile:
COUPLE/DRIFT (boot 0 per the M4 spec), MORPH (physical fader), capture
loops (M3), sampler audio content (SD card territory, as in the
original).

**Factory state** (used verbatim for: first boot, settings-read failure,
version mismatch, and factory reset): engines = synth/synth; targets
PITCH + LEVEL active, all others inactive; bases LEVEL 0.8, all others
0.5; all per-target depths 0.5; global scale = the scales spec's boot
default, mode SCALE both parts; internal tempo **100 BPM**; FX and voice
defaults exactly as the M1.6/M2 boot-default tables. This is the
"power on, hear something" guarantee: with the part's physical DEPTH
knob up, a fresh unit plays a quantized generative melody out of the
box.
**Factory reset gesture:** BOOT button held during power-on (distinct
from the runtime BOOT-3 s bootloader gesture) rewrites the settings
block with the factory state; both rings flood once as confirmation.

Write policy: dirty flag + ~2 s debounce after the last change, written
from the main loop (QSPI writes block — never in the audio callback).
The main loop serializes from a **snapshot under a sequence counter**
(retry if the audio thread mutated mid-copy) so a torn state is never
persisted. During the QSPI sector erase (tens of ms) pad polling is
blind — accepted and documented, it follows a 2 s idle period by
construction. The serialized struct is defined in `shell/` (plain POD +
version), so the desktop tests cover the roundtrip, the mismatch path
and the factory table.

## Desktop scenario host

New scenario actions, routed through the Shell instead of directly into
`Instrument`: `pad_touch` / `pad_release` {pad}, `pot` {id, value},
`switch` {id, pos}, `fader` {value}, `gate_in` {part}, `midi_note`
{ch, note}, `clock_pulse`. Existing direct-Instrument actions stay valid
(all existing scenarios remain pinned and untouched).

New outputs:

- `params.csv` — every Instrument setter call with timestamp: the primary
  assert target for gesture tests.
- `leds.csv` — scenario-declared probe LED indices at the full ~62 Hz
  render rate plus whole frames at 5 Hz (full frames at 62 Hz would be
  absurdly large).

Determinism: byte-identical reruns, as established since M1.

## Testing

Doctest on the desktop, all against `shell/`:

- gesture state machine: tap vs hold thresholds, release aborts,
  knob-moved-cancels-toggle, first held wins, ALT precedence,
  engine-switch countdown + early-release abort (incl. stop-recording
  rule), layer latch enter / ALT-tap exit / cross-layer switch, SPOT tap
  vs DRIFT hold vs fader-moved suppression, TAP tempo vs SETTLE hold
- takeover: pickup semantics, dead-zone display state after edits, TUNE
  borrow/return in the FLUX layer
- layer knob routing: which pot writes which parameter in every layer;
  sampler-content pad gestures (clear/save) only inside the latched
  sampler edit layer
- scales: ALT+TUNE relative stepping (global, both parts retune), 3-zone
  mode knob incl. hysteresis at zone boundaries
- CV combination law: `clamp01(knob + cv)` on RATE/DEPTH bases, CV never
  reaches layered values, V/Oct offset summed pre-quantizer at latch
- switch decode (3-position mapping) incl. the middle-position constants
  (EVOLVE 0.3/1.0, SMOOTH floor 0.35)
- LED priority chain on probe pixels (inspect > layered > countdown >
  record > capture > default), LEVEL edge segment, probability dimming,
  latched-layer ALT blink, internal-beat clock LED
- settings block roundtrip; version mismatch → factory state; factory
  table audibility invariant (LEVEL base > 0, PITCH active); snapshot
  consistency under concurrent mutation; write debounce
- panic: SETTLE pulls FLUX FEEDBACK bases > 0.95 down; master soft-clip
  bounds output for feedback 1.1 + shimmer worst case (render-host test)

Integration: a `gesture_tour.json` scenario (arm → record → capture →
FX layer → engine switch) asserted against a golden `params.csv`
sequence, plus determinism.

## Error behavior

- SD card missing/broken → sampler part runs empty, LED signals it, no
  crash (master-spec rule).
- Settings read failure / version mismatch → defaults.
- MPR121 init failure → pads dead but audio keeps running; logged.
- External clock loss → fall back to internal tempo at the last known
  BPM.

## Acceptance criteria

- `shell/` compiles with no libDaisy include and no heap; all shell unit
  tests pass on the desktop.
- The scenario host drives the full path panel event → shell →
  Instrument; `gesture_tour.json` reproduces its golden `params.csv`
  byte-identically.
- LED priority chain verified on probe pixels for all five ring states.
- Settings roundtrip + mismatch→defaults tested on the desktop.
- `make APP=mod` and `make APP=stock` both build (arm-none-eabi) without
  hardware present; `METER` compiled in; `arm-none-eabi-size` confirms
  text/bss fit the SRAM_EXEC / SRAM linker regions with stated headroom.
- Every UI debt deferred by M1.6–M5 is either wired in this design or
  explicitly listed as out of scope (serial routing, the mode-switch
  extension, and mid-session SD browsing beyond the per-part slot files
  stay out per this spec and the master spec).

## On-hardware checklist (deferred until a device exists)

The only genuinely hardware-bound work; everything above ships tested
without it:

- **first measurement:** METER with reverb + shimmer alone (the
  SDRAM-cache unknown) — before the full patch; decides the half-rate
  reverb lever
- tune gesture thresholds (~250/300 ms taps, ≥ 1 s holds) by feel
- verify LED positions/brightness (esp. CYCLE windows and the LEVEL edge
  segment), colors and gamma on the real WS2812 chain
- verify the third per-side switch is truly 3-position (UX review says
  yes)
- DAC CV out scaling against a tuner / oscilloscope, gate levels
- run the inherited calibrator (CV offsets, V/Oct references)
- SD card load/save through the inherited storage path
- MIDI clock + note-in over USB
- `METER`: confirm < 70 % worst case (both parts synth, 8 voices, FX,
  shimmer on)

## Roadmap placement

Milestone **M6 — firmware shell + hardware mapping**, last in the build
order, implemented after M3 (capture), M4 (center) and M5 (sampler
adapter) whose engine APIs this spec consumes as specified.
