# SYNC / COUPLE redesign — one switch, two worlds

**Date:** 2026-07-16
**Status:** approved (brainstorm with Bastian, 2026-07-16)

## Problem

The instrument currently has two overlapping, mutually ignorant time systems:

1. **Per-part tempo sync** — a near-invisible 3-way toggle (`Free / Sync / Triplet`)
   per part; in Sync the RATE knob snaps to 9 straight divisions of TEMPO.
2. **COUPLE** — a global Kuramoto PLL between the banks. It pulls FREE banks
   toward the geometric mean of their base rates and phase-locks them (hard at
   1.0), but knows nothing about tempo or musical ratios.

Symptoms: with COUPLE=1 the two RATE knobs can show unrelated values while the
lock silently overrides them; the TEMPO knob hides inside the ROOM (reverb)
block; nobody who doesn't know the instrument can predict what "sync" means.
Additionally the RST panel jack is dead — never read by the host — and even in
Sync mode only rates match the external clock, never the downbeat phase.

## Design

### 1. Interaction model — one prominent switch, two worlds

A single global **SYNC** switch replaces both per-part 3-way toggles.

**SYNC on (grid world):**
- The PITCH lane (melody + steps) of both parts runs hard on the tempo grid:
  RATE snaps to a musical division ladder, and the pitch phase is servo-locked
  to the transport phase (internal tempo or external clock).
- COUPLE becomes a *texture* control governing only the four non-pitch mod
  lanes: 1.0 = lockstep between the banks, lower values let the textures
  breathe apart Kuramoto-style while the melodies stay in sync.
- DRIFT's rate tap likewise affects only the mod lanes; its tune tap
  (±25 cents) stays active everywhere — detune is tuning, not timing.

**SYNC off (organic world):**
- Today's behavior: RATE is continuous 0.02–30 Hz, COUPLE acts on everything
  (including the melody phase; hard pairwise lock at 1.0 as fixed 2026-07-15).
- New: COUPLE is **zoned**.
  - 0–0.5: pure pairwise Kuramoto, no tempo awareness — the existing organic
    pull, character untouched.
  - 0.5–1.0: *grid gravity* fades in — the coupled target is additionally
    pulled toward the nearest musical division of the current tempo (rate and
    phase). This is the "shuffle" region: almost snapped, still elastic.
  - 1.0: hard lock on rate *and* downbeat — free mode converges to what SYNC
    mode does, so flipping the switch at full COUPLE is seamless.
  - The zones describe only when grid gravity joins; the pairwise pull itself
    keeps today's curve over the full 0–1 range (soft Kuramoto scaling with
    COUPLE, hard pairwise lock at 1.0). Nothing is re-mapped into 0–0.5.

### 2. Engine — transport phase (new)

A beat/bar phase accumulator in the engine core:

- Advanced from the tempo BPM at control rate; bars assume 4/4 for the
  ≥ 1-bar divisions.
- **CLK**: the host reports external clock edges to the engine (new API, e.g.
  `Instrument::clock_pulse()`); edges both re-measure BPM (as today) and align
  the transport beat phase.
- **RST**: `Instrument::reset_transport()` — resets the bar/downbeat. This
  gives the currently dead RST jack its job.
- Used by: SYNC-mode pitch phase servo, and free-mode grid gravity
  (COUPLE > 0.5).

### 3. Engine — rate-scale split

`SuperModulator::set_rate_scale(s)` (one multiplier for the whole bank) splits
into independent pitch-lane and mod-lane multipliers (e.g.
`set_rate_scale(pitch_s, mod_s)`), so the Center can:

- SYNC on: keep the pitch lane immune (scale 1.0 + transport servo) while
  COUPLE/DRIFT wander only shapes the mod-lane multiplier. At COUPLE=1 the
  wander is fully suppressed → mod lanes hold exact lane ratios (lockstep);
  below 1, EVOLVE/DRIFT wander passes through scaled by (1 − COUPLE).
- SYNC off: both multipliers driven together (today's behavior) plus the
  zoned grid gravity above 0.5.

`SyncMode { Sync, SyncTriplet, Free }` per part goes away; the engine gets a
global sync flag. Triplets survive as ladder entries (below), not as a mode.

### 4. RATE ladder (SYNC on)

Sorted by speed, dotted/straight/triplet interleaved — 17 detents:

```
8bar 4bar 2bar 1bar 1/2. 1/2 1/4. 1/2T 1/4 1/8. 1/4T 1/8 1/16. 1/8T 1/16 1/16T 1/32
```

(Strictly speed-sorted so the knob is monotonic — 1/2T (0.75 cpb) is faster than 1/4. (0.667 cpb).)

Per part — one part on 1/4 against the other on 1/8T replaces the old
triplet-mode trick. The VCV tooltip shows the division name. SYNC off: the
same knob is the continuous free-Hz control. Free-mode grid gravity
(COUPLE > 0.5) snaps to the nearest entry of this same ladder.

### 5. Panel & host (VCV)

Layout "A" from the mockup round:

- New labeled **TIME** group directly under MORPH: SYNC switch (prominent,
  2-position), TEMPO, COUPL. The couple knob sits next to the switch whose
  behavior it shapes.
- SCALE and DRIFT close ranks into the freed row; SPOT/DRIVE/SETL unchanged.
- ROOM keeps its 7 knobs (SIZE MIX DECAY / TONE DIFF / SMEAR MOD) — TEMPO
  moves out.
- The two per-part 3-way sync toggles at the button-strip edges are removed.
  Net control count: −1 (hardware-port constraint: prefer fewer controls).
- Changes flow through `res/gen_panel.py` (single source of panel truth),
  `Spotymod.cpp` (param table, SYNC switch, RST/CLK handling, RATE tooltips
  switching between division names and Hz), and the init defaults
  (`defaultFor()` / `configControls()`: SYNC defaults to on, replacing the
  per-part Synced defaults).

### 6. Verification

Engine tests (`tests/`):
- Transport phase tracks internal tempo; realigns on clock_pulse();
  reset_transport() zeroes the downbeat.
- SYNC on: pitch-lane rate is exactly the selected division; unaffected by
  COUPLE and DRIFT; both banks' pitch phases converge to the transport.
- SYNC on: mod-lane wander magnitude scales with (1 − COUPLE); zero at 1.0.
- SYNC off: below COUPLE 0.5 behavior matches today's pairwise pull
  bit-for-bit with grid gravity provably absent; at 1.0 the pair sits on a
  ladder rate and the downbeat.

Listening checks: render scenarios for both worlds; a VCV build to play.

## Out of scope

- Any reverb/ROOM parameter changes (layout shift only).
- Hardware firmware work itself — but the control-count constraint is binding.
