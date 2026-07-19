# DUST Phase A — listening brief

**Branch:** `dust-explore` (19 + 4 commits off `main` at `315bd78`)
**Plugin:** built and installed 2026-07-19 18:56 → `%LOCALAPPDATA%/Rack2/plugins-win-x64/Spotymod/`
**Suite:** 363/363, `test_panel.py` exit 0

Two new knobs per part in the FX box's bottom row: **DUST** (amount) and **ROT**
(character). TRIGGER pads are unchanged — the FRZ pad belongs to Phase C.

**What is built:** zone S (synced stutter grid) and zone F (free scatter),
forward read-only grains, the head takeover above DUST ≈ 0.7.
**What is not:** reverse grains as a tuned feature, writeback, freeze/erosion.
ROT above 0.66 *does* produce reverse grains — reachable but unspecified and
untuned, so do not judge that region.

---

## The gate

The criterion is the spec's own: **"a performance effect played live like the
delay."** Not *does it sound nice* — *can you play it?*

Setup: a patch you actually like, FLUX running and audible, SYNC on, hands on
DUST and ROT, monitors low. The mechanical properties (DUST = 0 bit-exact,
click-free grain births) are unit tests and are deliberately not your job.

| | Gesture | Decides | On failure |
|---|---|---|---|
| **G1** | Blind DUST sweep 0 → 1 → 0, twice | **the feature** | **STOP** |
| G2 | ROT travel 0 → 0.66 | zone breakpoints | tuning only, note it and continue |
| **G3(a)** | Land a stutter fill on purpose | **the feature** | **STOP** |
| G3(b) | Reach back for material older than the delay time | whether zone F exists | cut zone F, continue |

**G1 — the blind sweep.** Without looking at the screen, DUST 0 → 1 → 0, slowly,
while the music runs. Then do it **again** — the second pass is the one that
matters, and it was broken until this morning (see F1 below).
*Pass:* every position usable, 0 returns exactly the old delay, no level jump.
*Fail:* a dead range, a cliff into noise, a level jump, or a setting you cannot
get back from.

**G2 — the character travel.** DUST parked where G1 felt best. ROT 0 → 0.66.
*Pass:* reads as travelling — stutter tightening, loosening, opening into scatter.
*Fail:* an audible seam at 0.33, or two modes with nothing between.

**G3(a) — the stutter fill.** On purpose, without looking, land a rhythmic
stutter that locks to the delay. *Pass:* repeatable. *Fail:* only by hunting.

**G3(b) — the reach into the past.** On purpose, pull back material audibly
**older than the delay time**. *Pass:* identifiably older material appears and
you can steer roughly how far back. *Fail:* everything smears into texture where
old and new are indistinguishable — in which case zone F earns nothing that ROOM
does not already do, and the design collapses to zones S and R.

---

## Three things that will mislead you if nobody warns you

These are **not bugs**. They are tuning and mapping choices that produce a
musical result you would naturally blame on the *concept* when the cause is the
*mapping*. Found by the whole-branch review, specifically because the same shape
had already bitten once: a normalisation error made the top of the DUST knob get
quieter, which would have read as "the effect is dull at maximum".

**1. Zone F's spray is mapped linearly in seconds**, `0.15 s → 5.46 s`. By 20 %
into the zone the spray is already 1.1 s, and at the top only ~3 % of grains land
within 150 ms of the head. So most of zone F will sound like **one** texture.
If G2 feels like "FREE has no gradation", that is the linear mapping — it wants
to be exponential. Judge the *endpoints* of zone F, not the travel.

**2. Zone S produces doubling, not repeat.** The spray is 50 ms at 1× forward
playback, so each grain starts at a random offset within the last 50 ms and plays
*forward* — reading successive live material rather than replaying one slice. At
low-to-mid DUST the Hann windows still give audible grid-rate rhythm and this
works. At **high** DUST — exactly where G3(a) puts the knob, since fire
probability follows DUST — three grains per slot at 80–400 ms over a 125 ms grid
overlap into a chorus smear. Compounding it, DUST > 0.7 fades the echo head out
entirely, so at ROT = 0 and DUST = 1 the delay disappears and is replaced by a
smeared copy of the dry input.
**Try G3(a) at moderate DUST first.** If it only works there, the finding is
"zone S needs grid-quantised offsets", not "the sync zone doesn't stutter".

**3. Total wet energy drops across the top third of DUST.** The head takeover
fades the echo out equal-power while the grain sum is held at constant level by
the active-count normalisation, so the echo's share simply leaves. That is
spec-conformant (§2: "the cloud eats the delay") but sits against the same
section's "DUST changes texture and density, not loudness". Do not score the top
of the sweep as a dead range on that basis alone — check whether the *character*
keeps developing.

---

## What changed this morning that you should specifically re-check

**F1 — the grain pool was not cleared when DUST returned to 0.** Grains kept
`alive`, their age and their absolute tape index; the next rise resumed them
mid-window against a write head that had moved on. Any repeat of G1 entered the
up-stroke with a full pool — up to 8 grains at near-unity for ~400 ms of stale
material, exactly where you expect one splinter. Fixed, and now covered by a test
that fails against the old behaviour. **This is why G1 asks for two passes.**

**F2 — zone F reached 91.5 % of the tape, not all of it** (a bare `5.0 f` where
the tape is 5.46 s). Now derived from `Flux::kMaxSamples`. Relevant to G3(b): the
reach is genuinely the full tape now.

---

## Reporting back

For each gesture: pass/fail and one sentence. If something feels wrong but you
cannot name it, say what you *expected* to hear — that is usually enough to
locate it. Tuning constants all live in one block (`dust_tuning` in
`engine/fx/dust.h`), so "the stutter is too loose at the top" is directly
actionable.

If G1 or G3(a) fails, stop and say so — the remaining phases add character to
something that has none, and that is the answer the phase was built to get.
