# Website — VCV dev logs + "I already started" hero

**Date:** 2026-07-15
**Scope:** `website/index.html`, `website/styles.css`, new assets under `website/assets/site/web/`. No engine changes.

## Goal

Rorey (Synthux) is triaging ~80 applications. The page must trigger above the fold:
the applicant didn't wait for selection — Phase 1 is already in progress, public,
and **playable right now** in VCV Rack. Then three new dev-diary entries document
the latest milestones (VCV testbench, melody rework, reverb).

## 1. Hero rework — "I already started" (tone A + momentum strip C)

The hero keeps the existing layout system (`.section-grid`, copy left / media right)
and the H1. What changes:

- **Eyebrow:** `Synthux Residency 2026 application — Phase 1 already in progress`
- **Lead hook** (after the existing lead sentence, or as second paragraph):
  *"I didn't want to wait for the selection: the firmware is already twelve
  milestones deep on a public Spotykach fork — and you can play it in VCV Rack
  right now."* Commitment demonstrated, not claimed. No mention of the
  80-applicant email (private info, stays private).
- **Momentum strip:** a small mono-font band under the lead/actions:
  `12 milestones · 155 commits · 200 deterministic tests · playable now`
  (values re-verified at implementation time). Styled like `.firmware-legend` /
  eyebrow language: IBM Plex Mono, muted ink, dots as separators.
- **Actions:** new **primary** button "Play it in VCV Rack" → downloads
  `assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin` (`download` attribute).
  "See prototypes" demotes to secondary; GitHub link stays.
- **Hero media — maker past × playable present:** groovebox photo stays as the
  base (DIY charm). The generated `Spotymod.svg` panel floats over its lower-left
  corner in a small "app window" frame (thin chrome bar, dot buttons, label
  `Spotymod · VCV Rack`). CSS-positioned overlay of two real assets, no fabricated
  composite. On narrow viewports the panel card stacks below the photo.
- **Diary CTA upgraded:** `hero-status` link becomes a card-like row:
  pulsing dot + repo badge + **"Latest: it plays in VCV · 15 Jul — read the dev
  diary"** + chevron. Larger hit area, hover lift.

## 2. Three new dev-diary entries (appended after "The melody didn't sing"; `is-current` moves to the last)

### Entry 1 — "The workbench moved into the computer" (15 Jul)
The VCV Rack plugin as the permanent test surface until hardware arrives — and
for anyone who doesn't own a Spotykach. All important engine parameters are
exposed as knobs to discover which deserve real hardware knobs later; the plugin
stays a fixed part of development. Pays off the previous entry's "only playing
it does" lesson: that session became a fixture.

**Host-architecture SVG** (two-colour, site palette): one portable engine core
(green block) feeding two thin hosts — offline render host and VCV Rack module
(copper blocks) — with the caption logic "the engine doesn't know what a host
is; a new host is an afternoon, not a milestone." Inline `diary-breaker` style
or standalone SVG asset, matching the existing breaker figures.

CTA inside the entry: download the `.vcvplugin` (win-x64 now, Mac/Linux build
from source, link to `host/vcv`). Screenshot of the panel (the SVG itself
serves until a real Rack screenshot is provided).

### Entry 2 — "The melody sings now" (15 Jul)
M4.7 payoff, tested by playing in VCV. Gone: capture, probability dice, erode.
In their place the bipolar **MELODY** knob — LOOP centre, **GROW** right
(vary the phrase you have), **RENEW** left (structured new phrases from the
motivic generator, five switchable principles/characters via **PRIN**), plus the
per-step gate layer with live **DENSITY**. Composed, not diced. Proof: *play it*
— the entry links the plugin download instead of an offline render (deliberate:
the lesson of the previous entry was that renders don't prove musicality).

### Entry 3 — "The reverb that moonlights as a delay" (15 Jul)
Surprise winner: M4.8 dry/wet MIX (equal-power, clear-on-sleep bypass) +
M4.9 DIFFUSION replacing DEPTH. At low DIFFUSION the room falls apart into
discrete slap echoes — a delay in all but name; at high DIFFUSION attacks melt
into a full wash; DECAY past 100 % blooms. With MIX, SIZE (Doppler), DECAY,
TONE, DIFF all playable it behaves like an instrument, not an insert.

**Reverb-as-delay SVG** (two-colour breaker): left half discrete green impulse
taps decaying (delay character), right half the same energy smeared into a
copper wash — one knob between them (DIFF).

Proof: two audio renders exported from existing fork WAVs:
- `spotykach-reverb-delay.{mp3,ogg}` ← `build/wash_pluck_diff0625.wav` (echo character)
- `spotykach-reverb-wash.{mp3,ogg}` ← `build/wash_pluck_diff100.wav` (attacks melt)
Export via ffmpeg; if unavailable on this machine, flag and leave `<audio>`
blocks wired to the target filenames for the user to drop in.

## 3. "Current" updates

- Hero-status note + diary-meta note → `Latest: it plays in VCV · 15 Jul 2026`.
- Firmware-study intro paragraph: replace the stale "capture sequencer for
  melodies" clause with the MELODY-knob engine (capture was removed in M4.7).
- OG/meta description: add "playable in VCV Rack".
- Bump `styles.css` cache-buster query.

## 4. New assets committed to the site

| Asset | Source |
|---|---|
| `assets/site/web/spotymod-panel.svg` | copy of fork `host/vcv/res/Spotymod.svg` |
| `assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin` | fork `host/vcv/dist/` (137 KB) |
| `spotykach-reverb-delay.{mp3,ogg}` | fork `build/wash_pluck_diff0625.wav` |
| `spotykach-reverb-wash.{mp3,ogg}` | fork `build/wash_pluck_diff100.wav` |

Host-architecture and reverb SVGs are authored inline (diary-breaker figures),
in the existing palette: `#1d6f5f` green, `#b96532` copper, `#9b9385` baseline,
`#171713` ink.

## Non-goals / constraints

- No mention of the 80-applicant email anywhere on the page.
- No fabricated melody render — VCV is the proof.
- Panel SVG is generator output; never hand-edit the copy destined for the site
  beyond wrapping (re-copy from the fork if the panel changes).
- Keep the existing residency framing, H1, and section order; this is an
  amplification, not a redesign.
