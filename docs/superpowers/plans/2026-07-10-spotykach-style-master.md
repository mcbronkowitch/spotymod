# Spotykach Style Master Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and validate Concept 1 as an annotated study on unchanged Spotykach hardware, with the generated faceplate render used only as a speculative detail.

**Architecture:** Keep the official Spotykach photograph as an immutable HTML image layer and place an independent SVG annotation layer above it. Integrate that study into the website and present the generated product render separately with explicit speculative labeling.

**Tech Stack:** Built-in image generation, local image inspection, existing static HTML/CSS website assets.

## Global Constraints

- Preserve the physical Spotykach enclosure and its proportions.
- Keep every existing knob, large encoder, toggle switch, touchpad, jack and LED in exactly the same position and at the same scale.
- Do not add screens, controls, sockets or other hardware.
- Only the faceplate graphics, labels and implied firmware mapping change.
- Use a black panel, warm cream/gold screen-printed graphics, sparse petrol/teal accents and warm orange LEDs.
- Use a light neutral background with no hands, cables, surrounding instruments or studio clutter.
- Deliver Concept 1 only in this plan; Concepts 2 and 3 follow after explicit style approval.

---

### Task 1: Generate the Modulation Matrix style master

**Files:**
- Reference: `C:/Users/bernd/AppData/Local/Temp/spotykach-reference/overhead.jpg`
- Reference: `C:/Users/bernd/AppData/Local/Temp/spotykach-reference/zoom.jpg`
- Create: `assets/site/web/spotykach-modulation-matrix-style-master.png`

**Interfaces:**
- Consumes: Official Spotykach overhead and close-up product photographs.
- Produces: One landscape PNG that establishes composition, faceplate language, materials, lighting and typography for all later variants.

- [ ] **Step 1: Confirm both reference images are readable**

Run:

```powershell
Get-Item `
  'C:/Users/bernd/AppData/Local/Temp/spotykach-reference/overhead.jpg', `
  'C:/Users/bernd/AppData/Local/Temp/spotykach-reference/zoom.jpg' |
  Select-Object FullName, Length
```

Expected: Both files exist and have a non-zero `Length`.

- [ ] **Step 2: Generate one new referenced product study with the built-in image tool**

Use both images as reference images, not edit targets, with this prompt:

```text
Use case: product-mockup
Asset type: landscape faceplate concept study for a design-residency application website
Primary request: Create a new, highly legible Spotykach firmware faceplate study called Modulation Matrix. Preserve the referenced Spotykach hardware exactly: identical rectangular enclosure, proportions, and the exact position, count, scale and type of every existing knob, large encoder, toggle switch, touchpad, jack and LED. This is new firmware and faceplate artwork on existing hardware, not a hardware redesign.
Input images: Image 1 is the authoritative hardware-layout and overall-product reference. Image 2 is the material, control-detail and lighting reference.
Scene/backdrop: clean warm off-white studio surface matching an editorial portfolio website; no surrounding equipment
Subject: the complete Spotykach unit with two clearly mirrored modulation engines labeled MOD A and MOD B; large controls communicate RATE and MOTION; restrained smaller labels include SHAPE, RANGE, PROBABILITY and SMOOTH; toggle states include LOOP / EVOLVE, SYNC / FREE and STEP / FLOW; the existing twelve touchpads are presented as two modulation-target banks using PITCH, TIMBRE, DENSITY, SPACE, FEEDBACK and LEVEL; the center communicates COUPLE, DRIFT and MORPH
Style/medium: premium industrial-design faceplate visualization, tactile screen printing, directly related to the reference product's experimental abstract collage language but with original artwork
Composition/framing: landscape, complete instrument centered, frontal view from slightly above, near-orthographic geometry, generous margin, all controls and faceplate graphics visible
Lighting/mood: soft neutral studio lighting, calm and precise, subtle physical texture, warm orange LED rings
Color palette: matte black panel, warm cream and muted gold graphics, very sparse petrol/teal accents, warm orange indicators
Text: render only the short labels specified above plus the small identifier "SPOTYKACH / AMBIENT MODULATION FIRMWARE STUDY"
Constraints: hardware geometry and control placement are invariants; labels should be sparse and readable; the visual statement "modulation is the instrument" must be immediate
Avoid: added or removed controls, moved controls, screen, cables, hands, rack gear, props, dramatic perspective, dark background, illegible microtext, extra branding, watermark
```

Expected: One generated image showing the entire unchanged hardware layout with the new faceplate language.

- [ ] **Step 3: Inspect the generated image before accepting it**

Check visually:

```text
1. Complete enclosure visible and not cropped.
2. Mirrored MOD A / MOD B structure reads immediately.
3. Existing hardware count and placement match the references.
4. No new screen, knobs, switches, pads or jacks appear.
5. Black, cream/gold, petrol and orange palette fits both Spotykach and the application page.
6. The important labels are readable; no garbled paragraph text is present.
```

Expected: All six checks pass. If only one check fails, make one targeted image edit that changes only that defect and preserves everything else.

- [ ] **Step 4: Save the selected output into the project**

Copy the selected built-in output from its generated-images location to:

```text
C:/Users/bernd/Documents/AI/Synthux Design Residency/assets/site/web/spotykach-modulation-matrix-style-master.png
```

Expected: The final project file exists and the original generated output remains untouched.

### Task 2: Verify the project asset and request style approval

**Files:**
- Inspect: `assets/site/web/spotykach-modulation-matrix-style-master.png`

**Interfaces:**
- Consumes: The selected PNG from Task 1.
- Produces: A visually verified style master ready to guide Concepts 2 and 3.

- [ ] **Step 1: Confirm the asset format and dimensions**

Run a read-only image inspection and verify that the file is a valid PNG with landscape dimensions.

Expected: Width is greater than height, the image opens successfully, and the instrument remains legible at website scale.

- [ ] **Step 2: Display the final style master for review**

Show the project asset inline and state that it was generated with the built-in image tool from official product-photo references.

Expected: The user can approve the shared visual style or request one focused revision before Concepts 2 and 3 are created.

- [ ] **Step 3: Commit the approved style master**

Run only after explicit user approval:

```powershell
git add -- 'assets/site/web/spotykach-modulation-matrix-style-master.png'
git commit -m "Add Spotykach modulation matrix style study"
```

Expected: A commit containing only the approved image asset.
