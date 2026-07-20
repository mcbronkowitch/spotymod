# Website — VCV Dev Logs + "I Already Started" Hero Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the application site's hero into a commitment-demonstrating catcher (momentum strip, VCV plugin download, panel-over-groovebox media) and append three dev-diary entries (VCV testbench, melody rework, reverb-as-delay).

**Architecture:** Pure static-site change: `website/index.html` copy/structure + `website/styles.css` additions, plus new binary/SVG/audio assets copied or exported from the firmware fork at `C:\Users\bernd\Documents\AI\Spotykach`. No engine changes, no build system.

**Tech Stack:** Hand-written HTML/CSS (no framework), inline SVG figures, ffmpeg for audio export.

**Spec:** `docs/superpowers/specs/2026-07-15-website-vcv-devlogs-hero-design.md`

## Global Constraints

- **Repo:** `C:\Users\bernd\Documents\AI\Synthux Design Residency`, branch `main` (site work happens directly on main, matching all previous site commits).
- **Palette (verbatim from spec):** green `#1d6f5f`, copper `#b96532`, baseline grey `#9b9385`, ink `#171713`. Mono font stack: `"IBM Plex Mono", "Cascadia Mono", Consolas, monospace`. Serif headings are already global.
- **Never mention the 80-applicant email** anywhere on the page.
- **No fabricated melody render** — the melody entry's proof is the plugin download.
- **Panel SVG is generator output** — copy verbatim from the fork, never hand-edit the copy.
- **Momentum numbers (verified 2026-07-15 against the fork):** 12 milestones, 155 commits, 200 deterministic tests. If re-verification at implementation time differs, use the fresh numbers.
- **Entry dates:** all three new diary entries are `15 Jul 2026`. Order: VCV testbench → melody → reverb. `is-current` class moves to the reverb (last) entry.
- **Cache busting:** bump `styles.css?v=21` → `styles.css?v=22`.
- **No test suite exists for the website.** The verification cycle per task is: (a) `grep` assertions that the edit landed, (b) serve locally (`python -m http.server`) and eyeball in a browser at desktop and ~400px widths. Steps state exact greps.
- **Commit trailer** (every commit): `Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>` — never the default Claude/Anthropic trailer.
- **HTML conventions in this file:** apostrophes in diary prose are written `&#8217;`, em dashes are literal `—`, `<strong>` for panel-function words (knob/control names). Follow the surrounding entries.

---

### Task 0: Commit the leftover copy fix

`website/index.html` carries an uncommitted improvement to the 14 Jul "melody didn't sing" entry (Synthux-residency phrasing) from the previous session.

**Files:**
- Modify: none (commit existing working-tree change)

- [ ] **Step 1: Verify the diff is only that paragraph**

```bash
cd "C:/Users/bernd/Documents/AI/Synthux Design Residency"
git diff --stat website/
```

Expected: only `website/index.html`, ~8 lines churn. If anything else shows up, stop and surface it.

- [ ] **Step 2: Commit**

```bash
git add website/index.html
git commit -m "docs(site): melody-rework entry — residency builds the second pair of ears in twice over

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 1: Assets — panel SVG, plugin download, reverb audio exports

**Files:**
- Create: `website/assets/site/web/spotymod-panel.svg` (copy)
- Create: `website/assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin` (copy, ~137 KB)
- Create: `website/assets/site/web/spotykach-reverb-delay.mp3` / `.ogg` (export)
- Create: `website/assets/site/web/spotykach-reverb-wash.mp3` / `.ogg` (export)

**Interfaces:**
- Consumes: fork files `host/vcv/res/Spotymod.svg`, `host/vcv/dist/Spotymod-2.0.0-win-x64.vcvplugin`, `build/wash_pluck_diff0625.wav`, `build/wash_pluck_diff100.wav` (all verified present 2026-07-15).
- Produces: the exact asset paths above — Tasks 2, 4 and 5 reference them verbatim.

- [ ] **Step 1: Copy panel SVG and plugin**

```bash
cd "C:/Users/bernd/Documents/AI/Synthux Design Residency"
cp "/c/Users/bernd/Documents/AI/Spotykach/host/vcv/res/Spotymod.svg" website/assets/site/web/spotymod-panel.svg
cp "/c/Users/bernd/Documents/AI/Spotykach/host/vcv/dist/Spotymod-2.0.0-win-x64.vcvplugin" website/assets/site/web/
```

- [ ] **Step 2: Export the two reverb renders to mp3 + ogg**

```bash
cd "C:/Users/bernd/Documents/AI/Synthux Design Residency/website/assets/site/web"
SRC=/c/Users/bernd/Documents/AI/Spotykach/build
ffmpeg -y -i "$SRC/wash_pluck_diff0625.wav" -codec:a libmp3lame -q:a 2 spotykach-reverb-delay.mp3
ffmpeg -y -i "$SRC/wash_pluck_diff0625.wav" -codec:a libvorbis  -q:a 5 spotykach-reverb-delay.ogg
ffmpeg -y -i "$SRC/wash_pluck_diff100.wav"  -codec:a libmp3lame -q:a 2 spotykach-reverb-wash.mp3
ffmpeg -y -i "$SRC/wash_pluck_diff100.wav"  -codec:a libvorbis  -q:a 5 spotykach-reverb-wash.ogg
```

(ffmpeg 8.1.1 is at `C:\Users\bernd\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_...\bin` and on PATH — verified.)

- [ ] **Step 3: Verify**

```bash
ls -la spotymod-panel.svg Spotymod-2.0.0-win-x64.vcvplugin spotykach-reverb-delay.* spotykach-reverb-wash.*
```

Expected: 6 files, none zero-length; `.vcvplugin` ≈ 137 KB. Play one mp3 to confirm audio survived the export (or check `ffprobe` duration ≈ 24 s).

- [ ] **Step 4: Commit**

```bash
cd "C:/Users/bernd/Documents/AI/Synthux Design Residency"
git add website/assets/site/web/spotymod-panel.svg "website/assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin" website/assets/site/web/spotykach-reverb-delay.mp3 website/assets/site/web/spotykach-reverb-delay.ogg website/assets/site/web/spotykach-reverb-wash.mp3 website/assets/site/web/spotykach-reverb-wash.ogg
git commit -m "assets(site): Spotymod panel SVG, win-x64 .vcvplugin download, reverb delay/wash renders

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 2: Hero rework — copy, momentum strip, download button, panel-over-photo media, diary CTA

**Files:**
- Modify: `website/index.html` (hero section, lines ~36–81; stylesheet link line 18)
- Modify: `website/styles.css` (append hero additions; small edits near `.hero-status`)

**Interfaces:**
- Consumes: `assets/site/web/spotymod-panel.svg`, `assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin` (Task 1).
- Produces: CSS classes `.hero-momentum`, `.hero-panel-card`, `.hero-panel-chrome`, `.hero-status--loud` — Task 6 does not depend on them, but the diary CTA copy set here ("it plays in VCV · 15 Jul") must match Task 5's diary-meta note.

- [ ] **Step 1: Update eyebrow, lead hook, and buttons in `index.html`**

Replace the eyebrow line:

```html
          <p class="eyebrow">Synthux Residency 2026 application — Phase 1 already in progress</p>
```

After the existing `<p class="lead">…</p>` add:

```html
          <p class="lead lead-hook">
            I didn&#8217;t want to wait for the selection: the firmware is already
            twelve milestones deep on a public Spotykach fork — and you can play
            it in VCV Rack right now.
          </p>
```

Replace the `hero-actions` block (keep `id="links"`) with:

```html
          <div class="hero-actions" id="links">
            <a class="button primary" href="assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin" download>
              Play it in VCV Rack
              <svg class="button-icon" viewBox="0 0 12 8" aria-hidden="true">
                <path d="M1.5 1.5 L6 6 L10.5 1.5" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>
              </svg>
            </a>
            <a class="button secondary" href="#prototypes">See prototypes</a>
            <a class="button secondary" href="https://github.com/mcbronkowitch/spotymod" target="_blank" rel="noreferrer">
              <svg viewBox="0 0 16 16" width="14" height="14" fill="currentColor" aria-hidden="true"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"/></svg>
              The fork on GitHub
            </a>
          </div>

          <p class="hero-momentum" aria-label="Progress since forking on 10 July">
            <span>12 milestones</span>
            <span>155 commits</span>
            <span>200 deterministic tests</span>
            <span class="hero-momentum-hot">playable now</span>
          </p>
```

Note: the old secondary button linked `github.com/mcbronkowitch/chords` ("Earlier firmware on GitHub"). The hero now points at the *active* fork `spotymod`; the chords link still exists later in the page (case study 02), so nothing is lost.

- [ ] **Step 2: Upgrade the hero-status diary CTA**

Replace the `hero-status-note` line inside the `.hero-status` anchor:

```html
            <span class="hero-status-note">Latest: it plays in VCV · 15 Jul — read the dev diary</span>
```

and add the modifier class on the anchor: `class="hero-status hero-status--loud"`.

- [ ] **Step 3: Layer the panel over the groovebox photo**

Replace the entire `<figure class="hero-media">…</figure>` with:

```html
        <figure class="hero-media">
          <img
            src="assets/site/web/groovebox-overview.jpg"
            alt="A large hybrid groovebox prototype on a blue electronics work mat."
          >
          <a class="hero-panel-card" href="#dev-diary" aria-label="The Spotymod VCV Rack module — read the dev diary">
            <span class="hero-panel-chrome" aria-hidden="true">
              <span class="hero-panel-dot"></span><span class="hero-panel-dot"></span><span class="hero-panel-dot"></span>
              Spotymod · VCV Rack
            </span>
            <img src="assets/site/web/spotymod-panel.svg?v=1" alt="">
          </a>
          <figcaption>
            From this workbench to your computer: the hand-built groovebox that
            taught me where a solo project breaks — and the Spotykach firmware
            it led to, playable today as a VCV Rack module.
          </figcaption>
        </figure>
```

- [ ] **Step 4: Bump the stylesheet cache-buster**

Line 18: `styles.css?v=21` → `styles.css?v=22`.

- [ ] **Step 5: Append hero CSS to `styles.css`**

Add after the existing `.hero-media img { … }` rule block (line ~320), before `figcaption`:

```css
/* --- Hero: momentum strip + panel-over-photo card + loud diary CTA --- */
.hero-momentum {
  display: flex;
  flex-wrap: wrap;
  gap: 6px 0;
  margin: 22px 0 0;
  font-family: "IBM Plex Mono", "Cascadia Mono", Consolas, monospace;
  font-size: 12px;
  font-weight: 700;
  letter-spacing: 0.06em;
  color: var(--ink-soft);
}

.hero-momentum span + span::before {
  content: "·";
  margin: 0 10px;
  color: var(--muted);
}

.hero-momentum-hot {
  color: var(--copper);
}

.hero-media {
  position: relative;
}

.hero-panel-card {
  position: absolute;
  left: clamp(-30px, -3vw, -12px);
  bottom: 52px;
  width: min(66%, 360px);
  display: block;
  border: 1px solid rgba(23, 23, 19, 0.22);
  border-radius: 8px;
  overflow: hidden;
  background: var(--white);
  box-shadow: var(--shadow-lg);
  transition: transform 0.18s ease, box-shadow 0.18s ease;
}

.hero-panel-card:hover,
.hero-panel-card:focus-visible {
  transform: translateY(-3px);
}

.hero-panel-card:focus-visible {
  outline: 2px solid var(--solder-green);
  outline-offset: 3px;
}

.hero-panel-chrome {
  display: flex;
  align-items: center;
  gap: 5px;
  padding: 5px 9px;
  border-bottom: 1px solid rgba(23, 23, 19, 0.14);
  background: var(--paper-deep);
  font-family: "IBM Plex Mono", "Cascadia Mono", Consolas, monospace;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  color: var(--muted);
}

.hero-panel-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: rgba(23, 23, 19, 0.18);
}

.hero-panel-chrome .hero-panel-dot:first-child {
  background: var(--copper);
}

.hero-panel-card img {
  display: block;
  width: 100%;
  height: auto;
  border: 0;
  border-radius: 0;
  box-shadow: none;
}

.hero-status--loud {
  padding: 12px 16px;
  border: 1px solid rgba(23, 23, 19, 0.16);
  border-radius: 12px;
  background: var(--white);
  box-shadow: var(--shadow-sm, 0 1px 2px rgba(23, 23, 19, 0.06));
  transition: transform 0.15s ease, border-color 0.15s ease;
}

.hero-status--loud:hover {
  transform: translateY(-1px);
  border-color: var(--solder-green);
}

.hero-status--loud .hero-status-note {
  color: var(--ink);
  font-weight: 700;
}

@media (max-width: 900px) {
  .hero-panel-card {
    position: static;
    width: 100%;
    margin-top: 14px;
  }
}
```

Check first whether `--shadow-sm` exists in `:root` (grep `shadow-sm`); if not, use the literal fallback shown above (the `var(--shadow-sm, …)` form is safe either way).

- [ ] **Step 6: Verify**

```bash
cd "C:/Users/bernd/Documents/AI/Synthux Design Residency"
grep -c "hero-momentum\|hero-panel-card\|Play it in VCV Rack\|styles.css?v=22" website/index.html
grep -c "hero-momentum\|hero-panel-card\|hero-status--loud" website/styles.css
python -m http.server 8080 --directory website
```

Expected: both greps > 0. Open `http://localhost:8080` — desktop: panel card overlaps the photo's lower-left, momentum strip reads on one or two lines, download button first; ~400 px width: panel card stacks below the photo, no horizontal scroll. Kill the server after checking.

- [ ] **Step 7: Commit**

```bash
git add website/index.html website/styles.css
git commit -m "site(hero): 'Phase 1 already in progress' — momentum strip, VCV download, panel-over-workbench media, loud diary CTA

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 3: Diary entry — "The workbench moved into the computer" (VCV testbench + host-architecture SVG)

**Files:**
- Modify: `website/index.html` (append `<li>` after the "The melody didn&#8217;t sing" entry, i.e. after its closing `</li>` — currently the last child of `ol.diary-timeline`)
- Modify: `website/styles.css` (one small rule for the entry's panel figure + download link)

**Interfaces:**
- Consumes: `assets/site/web/spotymod-panel.svg`, `assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin` (Task 1).
- Produces: CSS class `.diary-panel` and `.diary-download`; the entry sits before Task 4's entry.

- [ ] **Step 1: Remove `is-current` from the melody entry**

The `<li class="is-current">` (14 Jul, "The melody didn&#8217;t sing") loses the class → plain `<li>`. (Task 5 sets `is-current` on the final new entry.)

- [ ] **Step 2: Append the new entry `<li>`**

```html
              <li>
                <span class="diary-date">15 Jul 2026</span>
                <div class="diary-entry">
                  <h4>The workbench moved into the computer</h4>
                  <p>
                    The last entry ended with a lesson: only playing the
                    instrument tells the truth. So the VCV Rack experiment
                    stopped being an experiment — <strong>Spotymod</strong> is
                    now a real Rack module and a permanent part of the
                    development loop, the test surface until real hardware is on
                    the bench. It exposes deliberately <em>too many</em> knobs:
                    every engine parameter that might matter gets one, and
                    playing decides which of them earn physical knobs on the
                    Spotykach — and which collapse into the macro gestures. And
                    it cuts both ways: anyone who doesn&#8217;t own the hardware
                    can play the instrument today.
                  </p>
                  <figure class="diary-panel">
                    <img src="assets/site/web/spotymod-panel.svg?v=1"
                      alt="The Spotymod VCV Rack panel: two mirrored banks of green and copper knobs around a shared center strip with MORPH, COUPLE, DRIFT, SPOT and the reverb room controls.">
                    <figcaption>The whole engine on one panel — part A in solder green, part B in copper, the shared room in the middle.</figcaption>
                  </figure>
                  <p>
                    Porting took an afternoon, and that was the plan all along.
                    The engine has been hardware-independent since milestone
                    M1: it doesn&#8217;t know what a host is — it reads
                    parameters, processes a block, returns audio. Deterministic,
                    no clocks, no I/O, no heap. The offline render host that
                    produced every demo on this page is one thin adapter around
                    that core; the Rack module is simply a second one. Same
                    engine, note for note — which means every knob you turn in
                    Rack is testing the exact code that will run on the
                    hardware.
                  </p>
                  <figure class="diary-breaker" aria-hidden="true">
                    <svg class="diary-breaker-env" viewBox="0 0 208 56" width="208" height="56">
                      <rect x="10" y="16" width="72" height="24" rx="3" fill="none" stroke="#1d6f5f" stroke-width="2"/>
                      <text x="46" y="26.5" fill="#1d6f5f" text-anchor="middle" font-family="IBM Plex Mono, Consolas, monospace" font-size="7" font-weight="700">ENGINE</text>
                      <text x="46" y="35.5" fill="#9b9385" text-anchor="middle" font-family="IBM Plex Mono, Consolas, monospace" font-size="5">no clocks · no I/O</text>
                      <path d="M82 22 C104 22 112 13 130 13" fill="none" stroke="#b96532" stroke-width="1.6" stroke-linecap="round"/>
                      <path d="M82 34 C104 34 112 43 130 43" fill="none" stroke="#b96532" stroke-width="1.6" stroke-linecap="round"/>
                      <rect x="130" y="4"  width="68" height="18" rx="3" fill="none" stroke="#b96532" stroke-width="1.6"/>
                      <text x="164" y="15.5" fill="#b96532" text-anchor="middle" font-family="IBM Plex Mono, Consolas, monospace" font-size="6" font-weight="700">RENDER → WAV</text>
                      <rect x="130" y="34" width="68" height="18" rx="3" fill="none" stroke="#b96532" stroke-width="1.6"/>
                      <text x="164" y="45.5" fill="#b96532" text-anchor="middle" font-family="IBM Plex Mono, Consolas, monospace" font-size="6" font-weight="700">VCV RACK</text>
                    </svg>
                    <figcaption class="diary-breaker-label">one core &#183; thin hosts &#183; a new host is an afternoon</figcaption>
                  </figure>
                  <p class="diary-download">
                    <a class="button primary" href="assets/site/web/Spotymod-2.0.0-win-x64.vcvplugin" download>Download Spotymod for VCV Rack</a>
                    <span>win-x64 · drop into Rack&#8217;s <code>plugins-win-x64</code> folder · Mac/Linux
                    <a href="https://github.com/mcbronkowitch/spotymod" target="_blank" rel="noreferrer">build from source</a></span>
                  </p>
                </div>
              </li>
```

- [ ] **Step 3: Add the two CSS rules**

Append to `styles.css` after the `.diary-breaker-label` rules:

```css
.diary-panel {
  margin: 22px 0;
}

.diary-panel img {
  width: min(560px, 100%);
  border: 1px solid rgba(23, 23, 19, 0.16);
  border-radius: 8px;
  background: var(--white);
  box-shadow: var(--shadow-lg);
}

.diary-download {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 10px 14px;
  margin: 20px 0 6px;
}

.diary-download span {
  color: var(--muted);
  font-size: 13px;
}
```

- [ ] **Step 4: Verify**

```bash
grep -c "The workbench moved into the computer\|diary-panel\|Download Spotymod" website/index.html
```

Expected: 3 hits total across those patterns (1 each). Serve and eyeball: entry renders after "The melody didn&#8217;t sing", SVG diagram legible, download button works (browser downloads the 137 KB file).

- [ ] **Step 5: Commit**

```bash
git add website/index.html website/styles.css
git commit -m "docs(site): dev-diary 'The workbench moved into the computer' — Spotymod VCV module, host-architecture figure, plugin download

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 4: Diary entry — "The melody sings now"

**Files:**
- Modify: `website/index.html` (append `<li>` after Task 3's entry)

**Interfaces:**
- Consumes: `.diary-download` CSS (Task 3), plugin asset (Task 1).
- Produces: the entry Task 5's entry follows.

- [ ] **Step 1: Append the entry `<li>`**

```html
              <li>
                <span class="diary-date">15 Jul 2026</span>
                <div class="diary-entry">
                  <h4>The melody sings now</h4>
                  <p>
                    The rework from the last entry is in (milestone M4.7), and
                    it was tested the only way that counts — played, in Rack,
                    for an evening. Gone for good: the capture gesture, the
                    probability dice, the erode-to-one-note half of entropy.
                    In their place a single bipolar <strong>MELODY</strong>
                    knob. Centre is <strong>LOOP</strong> — the phrase you have,
                    exactly. Turn right into <strong>GROW</strong> and the
                    phrase varies itself, one fired step at a time, without
                    losing its identity. Turn left into <strong>RENEW</strong>
                    and a motivic generator writes genuinely new phrases —
                    arcs, questions and answers, walks around the root — in
                    five switchable characters on the <strong>PRIN</strong>
                    button. Rhythm finally belongs to the melody too: a
                    per-step gate layer decides which steps sound at all, and
                    <strong>DENS</strong> thins or fills the line live.
                    Composed, not diced.
                  </p>
                  <p>
                    No offline render this time, on principle. The last entry
                    was one long lesson in how little a render proves about
                    whether a melody sings — so this one ships as the thing
                    itself. Download the module above, turn
                    <strong>MELODY</strong> slowly from hard left to hard
                    right, and listen to the same eight notes learn new
                    manners. That&#8217;s the proof.
                  </p>
                </div>
              </li>
```

- [ ] **Step 2: Verify**

```bash
grep -c "The melody sings now" website/index.html
```

Expected: 1. Serve and eyeball ordering: …didn&#8217;t sing → workbench → sings now.

- [ ] **Step 3: Commit**

```bash
git add website/index.html
git commit -m "docs(site): dev-diary 'The melody sings now' — MELODY knob (RENEW/LOOP/GROW), gate layer, proof by playing

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 5: Diary entry — "The reverb that moonlights as a delay" (+ `is-current`, diary-meta note)

**Files:**
- Modify: `website/index.html` (append final `<li class="is-current">`; update `diary-meta-note`)

**Interfaces:**
- Consumes: audio assets `spotykach-reverb-delay.{mp3,ogg}`, `spotykach-reverb-wash.{mp3,ogg}` (Task 1); `diary-breaker` CSS (existing).
- Produces: the diary's new latest entry; `diary-meta-note` copy that must match Task 2's hero-status note ("it plays in VCV · 15 Jul").

- [ ] **Step 1: Append the entry `<li>` (with `is-current`)**

```html
              <li class="is-current">
                <span class="diary-date">15 Jul 2026</span>
                <div class="diary-entry">
                  <h4>The reverb that moonlights as a delay</h4>
                  <p>
                    The surprise winner of the Rack evenings. Two small
                    milestones grew the shared room into a second instrument:
                    an equal-power <strong>MIX</strong> knob at the master join
                    (with a proper bypass — at zero the room empties and stops
                    burning CPU), and <strong>DIFF</strong>, which replaced the
                    old modulation-depth knob (milestones M4.8, M4.9).
                    DIFF is the revelation. Turned down, the room stops being a
                    room: the tail falls apart into discrete slap echoes — a
                    delay in all but name, and with <strong>SIZE</strong> as its
                    time knob a very playable one. Turned up, every attack melts
                    on entry and the same tank is a dense ambient wash.
                  </p>
                  <figure class="diary-breaker" aria-hidden="true">
                    <svg class="diary-breaker-env" viewBox="0 0 208 56" width="208" height="56">
                      <path class="diary-breaker-base" d="M10 46 L198 46" fill="none" stroke="#9b9385" stroke-width="1.2" stroke-dasharray="2 4" stroke-linecap="round" opacity="0.5"/>
                      <path d="M14 46 L14 12" stroke="#1d6f5f" stroke-width="2.4" stroke-linecap="round"/>
                      <path d="M30 46 L30 22" stroke="#1d6f5f" stroke-width="2.4" stroke-linecap="round"/>
                      <path d="M46 46 L46 30" stroke="#1d6f5f" stroke-width="2.4" stroke-linecap="round"/>
                      <path d="M62 46 L62 36" stroke="#1d6f5f" stroke-width="2.4" stroke-linecap="round"/>
                      <path d="M78 46 L78 41" stroke="#1d6f5f" stroke-width="2.4" stroke-linecap="round"/>
                      <circle cx="104" cy="30" r="6" fill="none" stroke="#171713" stroke-width="1.6"/>
                      <line x1="104" y1="30" x2="107.5" y2="25.5" stroke="#171713" stroke-width="1.6" stroke-linecap="round"/>
                      <path d="M124 44 C134 20 142 14 152 13 C168 12 184 26 198 42 L198 46 L124 46 Z" fill="#b96532" opacity="0.28"/>
                      <path d="M124 44 C134 20 142 14 152 13 C168 12 184 26 198 42" fill="none" stroke="#b96532" stroke-width="2" stroke-linecap="round"/>
                    </svg>
                    <figcaption class="diary-breaker-label">one DIFF knob &#183; slap echoes &#8596; full wash</figcaption>
                  </figure>
                  <p>
                    Together with <strong>DECAY</strong> past 100 % — where the
                    tank crosses into a self-sustaining bloom — the room now
                    plays like an instrument: ride MIX as a send fader, DIFF as
                    the material, SIZE as pitch-warping time, DECAY as a sustain
                    pedal that can hold a chord forever. Two renders below,
                    same patch, one knob apart: DIFF low, plucks answering
                    themselves in echoes — then DIFF full, the same plucks
                    arriving as weather.
                  </p>
                  <figure class="diary-render">
                    <figcaption class="diary-render-label">
                      <svg class="diary-render-wave" viewBox="0 0 20 12" width="20" height="12" aria-hidden="true">
                        <rect x="0" y="4" width="2" height="4" rx="1"/>
                        <rect x="4" y="1" width="2" height="10" rx="1"/>
                        <rect x="8" y="3" width="2" height="6" rx="1"/>
                        <rect x="12" y="0" width="2" height="12" rx="1"/>
                        <rect x="16" y="5" width="2" height="2" rx="1"/>
                      </svg>
                      Offline render · the delay it moonlights as
                    </figcaption>
                    <audio controls preload="metadata">
                      <source src="assets/site/web/spotykach-reverb-delay.mp3" type="audio/mpeg">
                      <source src="assets/site/web/spotykach-reverb-delay.ogg" type="audio/ogg">
                    </audio>
                  </figure>
                  <figure class="diary-render">
                    <figcaption class="diary-render-label">
                      <svg class="diary-render-wave" viewBox="0 0 20 12" width="20" height="12" aria-hidden="true">
                        <rect x="0" y="4" width="2" height="4" rx="1"/>
                        <rect x="4" y="1" width="2" height="10" rx="1"/>
                        <rect x="8" y="3" width="2" height="6" rx="1"/>
                        <rect x="12" y="0" width="2" height="12" rx="1"/>
                        <rect x="16" y="5" width="2" height="2" rx="1"/>
                      </svg>
                      Offline render · the wash it was hired for
                    </figcaption>
                    <audio controls preload="metadata">
                      <source src="assets/site/web/spotykach-reverb-wash.mp3" type="audio/mpeg">
                      <source src="assets/site/web/spotykach-reverb-wash.ogg" type="audio/ogg">
                    </audio>
                  </figure>
                </div>
              </li>
```

- [ ] **Step 2: Update the diary summary meta note**

```html
                <span class="diary-meta-note">Latest: it plays in VCV · 15 Jul 2026</span>
```

- [ ] **Step 3: Verify**

```bash
grep -c "moonlights as a delay\|spotykach-reverb-delay.mp3\|spotykach-reverb-wash.mp3" website/index.html
grep -c "is-current" website/index.html
```

Expected: first grep 3 (1+1+1); `is-current` exactly 1 (moved, not duplicated). Serve and eyeball: entry is last, copper accents active (is-current), both players play.

- [ ] **Step 4: Commit**

```bash
git add website/index.html
git commit -m "docs(site): dev-diary 'The reverb that moonlights as a delay' — MIX + DIFF, delay/wash renders; latest -> it plays in VCV

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

### Task 6: Current-state sweep — firmware-study intro, OG/meta

**Files:**
- Modify: `website/index.html` (head meta lines ~7–15; firmware-study intro paragraph line ~180)

**Interfaces:**
- Consumes: nothing new. Produces: nothing downstream — closes the plan.

- [ ] **Step 1: Firmware-study intro paragraph**

In the `#phase1` section replace the clause in:

```html
            Two parts, each driving five independent modulation lanes from one set of
            macro controls — with a capture sequencer for melodies and one layered center fader.
```

with:

```html
            Two parts, each driving five independent modulation lanes from one set of
            macro controls — with a motivic melody engine on one knob and one layered center fader.
```

- [ ] **Step 2: Meta/OG description updates**

`<meta name="description" …>` becomes:

```html
    <meta
      name="description"
      content="A visual companion to Bastian Tonk's Synthux Residency application: a focused generative ambient groovebox, playable today as a VCV Rack module, built in the open on a Spotykach fork."
    >
```

`og:description` becomes:

```html
    <meta property="og:description" content="A small ambient groovebox where modulation is the instrument. Twelve milestones in, public firmware, playable now in VCV Rack.">
```

- [ ] **Step 3: Verify + full-page pass**

```bash
grep -c "motivic melody engine\|playable today as a VCV Rack module\|playable now in VCV Rack" website/index.html
grep -c "capture sequencer for melodies" website/index.html
```

Expected: first 3, second 0. Serve once more and click through the whole page top to bottom (hero download, panel card link, diary open-from-hash, all three new entries, audio players, both viewports).

- [ ] **Step 4: Commit**

```bash
git add website/index.html
git commit -m "docs(site): current-state sweep — melody engine in the phase-1 intro, VCV-playable OG/meta copy

Co-Authored-By: HAL 9000 <293417720+bea-ton-k@users.noreply.github.com>"
```

---

## Post-plan notes

- **User by-ear/by-eye gate before pushing:** the hero is a first impression for a reviewer; the user should look at the served page (desktop + phone width) and listen to both exported renders before anything is pushed.
- **Deferred, deliberate:** real in-Rack screenshot (panel SVG serves until provided); Mac/Linux plugin builds (source link covers them); melody render (never — the plugin is the proof).
- **If the fork's panel changes** (new knobs), re-copy `Spotymod.svg` → `spotymod-panel.svg` and bump its `?v=`.
