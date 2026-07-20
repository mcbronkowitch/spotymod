# Spotykach Capture Sequencer — M3 Design

The capture/freeze layer for the modulation-first firmware: the PITCH lane's
last heard cycle can be frozen into a loop and replayed through the identical
downstream chain, turning the generative melody into a repeatable sequence
without leaving the modulation system. Extends the main design
(`2026-07-10-spotykach-modulation-first-synth-design.md`, section "Capture
sequencer (per part)"); milestone **M3**, after M2 (synth voice), before M4
(center section).

Fork: `github.com/mcbronkowitch/spotymod`, local at
`c:\Users\bernd\Documents\AI\Spotykach`.

## Goal

Deliver the "my sequence vs. the dice" half of the master spec: capture is a
source swap, not a separate sequencer. While replaying, the PITCH lane plays
its recorded cycle (pitch values + trigger pattern including probability
gaps) instead of computing wavetable + random, and everything behind the
source — probability dice, step/flow, SMOOTH, RANGE in the lane; base, depth,
TUNE, quantizer in `Part` — stays literally the same running code. Fully
testable on the desktop; the ALT+SEQ gesture and the ring step-pattern
display are M6 (engine API now, as established in M2).

## Decisions (from brainstorming, 2026-07-12)

- **Retroactive recording.** The PITCH lane records permanently into a
  rolling ring buffer; capture freezes the cycle you just heard — the gesture
  reacts to something that already happened, never to an unknown future
  phrase.
- **The full downstream chain stays live, including RANGE.** Capture swaps
  only the source; RATE (loop speed), PROBABILITY (thinning), SMOOTH
  (glides), RANGE (span, down to off), TUNE and the quantizer all keep acting
  on the loop. "Everything else stays literally the same system."
- **Suppressed trigger = hold.** When the live PROBABILITY dice fail at a
  recorded trigger, the step is skipped completely: no engine trigger AND the
  pitch (CV out, drone voice) holds the last fired value until the next
  fired trigger — matching the generative freeze behavior; thinning produces
  longer held notes, not silent pitch motion.
- **The loop survives replay-off (two buffers).** Replay toggle and capture
  are separate engine actions (`set_replay` / `capture_now`); "my sequence"
  survives any number of trips back to the dice, honoring the master spec's
  "switch at will" promise. Re-capture is an explicit action. Suggested M6
  gesture mapping: ALT+SEQ tap = replay toggle (first tap with no loop
  stored = capture + replay), ALT+SEQ hold = capture fresh — final gesture
  design is an M6 decision.
- **Architecture: standalone `CaptureLoop` + minimal lane hook** (approach B
  of the brainstorm). A small independently testable buffer class; `ModLane`
  gains only a narrow replay-source hook; `SuperModulator` owns exactly one
  `CaptureLoop` and wires it to `LANE_PITCH` only. No per-lane generality
  (YAGNI: the master spec captures only PITCH).
- **EVOLVE is ignored entirely on the replaying PITCH lane** (including the
  rate random walk — the loop stays metronomic); it keeps affecting the
  other four lanes. "EVOLVE affects only the live lanes; the captured loop
  itself never mutates."

## Behavior model

### CaptureLoop (new, `engine/mod/capture.h/.cpp`)

Two phase-indexed slot buffers of **192 slots per cycle** (192 divides the
usual step grids 8/12/16/24/32 evenly). Per slot: value (float, the lane's
pre-smooth `_target`) + fired flag. Memory ≈ 2 KB per part, static.

- **Rolling record buffer:** in generative mode the PITCH lane writes its
  `_target` and fired events into the slot the phase is passing through —
  the ring always holds the last heard cycle-length window. Recording pauses
  during replay (the generative source is not computed then) and resumes on
  replay-off.
- **Frozen loop buffer:** `capture_now()` copies ring → loop and marks the
  loop valid.
- **Retroactive seam:** freezing mid-cycle captures the last cycle-length
  *window* — slots ahead of the playhead still hold the previous pass. That
  is exactly "what you heard over the last cycle"; the seam sits at the
  capture point.
- **Init state:** value 0 (bipolar center) everywhere, fired flag on slot 0
  only — capturing before one full cycle has ever elapsed yields a held
  root-ish note, harmless.

### Replay rule (inside `ModLane`, replay on)

One uniform rule for STEP- and FLOW-recorded material, hooked exactly where
`_on_boundary()` rolls dice and latches today:

1. The phase advances as always (RATE, sync/free → loop speed live). When it
   enters a slot whose loop fired flag is set, the **current** PROBABILITY
   dice roll: pass → trigger fires (`_fired`) and the freeze lifts; fail →
   frozen (hold until the next passing trigger).
2. While not frozen, `_target` = the loop value at the current slot.
   STEP-recorded loops hold automatically (the recorded values ARE the held
   steps, including recording-time probability gaps); FLOW-recorded loops
   replay their continuous curve at slot resolution.
3. Downstream is untouched running code: SMOOTH → RANGE in the lane;
   base/depth/TUNE/quantizer in `Part`. Transposition and scale changes act
   live on the loop; `pitch_cv()` and the engine trigger path
   (`lane_fired`) follow automatically.
4. EVOLVE contributions are ignored on this lane while replaying (no
   `_ev_rate` on the phase, no `_ev_shape`/`_ev_phase` — no raw computation
   happens anyway). The wavetable/S&H code idles.

Toggling replay is phase-synchronous by construction — the phase never
stops, only the boundary-value source swaps. State is volatile: a power
cycle returns to generative with no loop (master spec).

## Module changes

All in `namespace spky`; no heap, no allocation in the audio path, no
libDaisy — as established.

| Module | Change |
|---|---|
| `engine/mod/capture.h/.cpp` | **new**: `CaptureLoop` — `record(slot, value, fired)`, `capture_now()`, `value(int slot)`, `fired(int slot)`, `valid()`, `kSlots = 192`. A dumb buffer, no state beyond the slots — testable in isolation. |
| `engine/mod/lane.h/.cpp` | narrow hook: `set_capture_loop(CaptureLoop*)` (wired once at init), `set_replay(bool)`, `replaying()`. Generative: records per slot into the ring. Replay: rule above instead of wavetable + `_compute_raw()`; boundaries come from the recorded fired slots (the live step grid does not apply to this lane while replaying); dice, smooth and range are the same code paths. Lanes without a `CaptureLoop*` (the other four) behave exactly as today. |
| `engine/mod/super_modulator.h/.cpp` | owns one `CaptureLoop`, wires it to `LANE_PITCH` in `init()`. API: `capture_now()`, `set_replay(bool)`, `replaying()`, `loop_valid()`. |
| `engine/instrument.h` | delegation in the existing style: `capture_now(int p)`, `set_replay(int p, bool)`, `replaying(int p)`, `loop_valid(int p)` → `_parts[p].mod()`. |

`Part` needs no change (it already exposes `mod()`; targets, quantizer and
trigger forwarding are untouched).

## Desktop render host

- Two new scenario actions, 1:1 with the API: `capture_now`, `set_replay`.
- `mods.csv` gains one column per part: `capture` (replay state 0/1).
- Demo scenario **`capture_loop.json`**: generative STEP melody (Dorian,
  probability ~0.6, synth engine) → `capture_now` + replay at t ≈ 20 s →
  the loop repeats identically for several cycles → probability thins it
  live → TUNE transposes → replay off (dice return) → replay on again →
  the **same** loop returns (the two-buffer promise, readable in the CSV).

## Testing

doctest, desktop, as established:

- **CaptureLoop in isolation:** slot mapping, ring→loop copy, seam semantics
  (window, not cycle start), init state.
- **Replay determinism:** probability 1 → every replay cycle yields the
  identical target sequence.
- **Retroactivity:** a known synthetic history → the frozen loop equals the
  last window at the capture point.
- **Live layers:** probability < 1 suppresses triggers and holds the pitch;
  SMOOTH glides; RANGE scales down to off; TUNE/scale requantize the loop
  (Part level).
- **EVOLVE isolation:** evolve > 0 while replaying → loop timing metronomic
  and content constant; the other lanes wander.
- **Persistence:** capture → replay off → generative runs on → replay on →
  the **same** loop.
- **Phase-sync toggle:** no jump beyond a normal boundary step.
- **Trigger path:** the recorded fired pattern starts synth voices via the
  existing `lane_fired` route.
- **Bit-determinism invariant:** identical scenario → bit-identical WAV.

Sound character (does the frozen melody sit naturally against the moving
lanes?) is verified by ear via the render.

## Budget

CPU negligible: per sample one slot compare + one array read (replay) or one
array write (recording); no transcendental math. Memory: 2 × 192 slots
(float + flag) ≈ 2 KB per part, static — no heap.

## Assumptions to verify during implementation

- 192 slots resolve FLOW curves finely enough after SMOOTH (else raise the
  slot count; memory is cheap here).
- The recording-pauses-during-replay rule keeps `capture_now` while
  replaying harmless (it re-freezes the pre-capture window — likely ≈ the
  same loop; confirm no surprise).
- The frozen-at-boundary interaction with STEP mode's grid switch (changing
  step count while replaying affects only the other lanes; the loop's
  trigger slots come from the recording) — confirm no aliasing between the
  live step grid and recorded slots.
- Gesture thresholds and the tap/hold mapping for ALT+SEQ are M6; the
  engine API must not need to change for either mapping variant.

## Acceptance criteria

- Master spec M3 criterion: after a capture event in the scenario, the PITCH
  loop repeats identically while the other lanes keep moving — visible in
  `mods.csv`.
- Persistence criterion: replay off → generative audibly resumes; replay on
  again → the identical loop returns (CSV-verifiable).
- Probability thinning on the loop produces held notes (no engine trigger,
  pitch held), and probability back to 1 restores the full loop.
- `engine/` still compiles with no libDaisy include; all new unit tests
  pass; existing tests stay green.
- Bit-determinism invariant holds.
