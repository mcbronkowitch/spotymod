# Synth-Engine-Erweiterung — Recherche

**Datum:** 2026-07-18
**Status:** Recherche, **keine Entscheidung**. Nichts hiervon ist beschlossen oder eingeplant.
**Frage:** Welche Open-Source-Synthese-Engines lassen sich realistisch auf den Daisy Seed
(STM32H750, 480 MHz, Cortex-M7, single-precision FPU, 64 MB SDRAM) portieren, bei 2 Parts × 4
Stimmen (= 8 Stimmen) plus Effekte? Sortiert nach neuen Klangfeatures.

---

## TL;DR

1. **Es gibt keine belastbaren CPU-Zahlen.** Die Recherche prüfte 25 Claims adversarial; 10 wurden
   widerlegt — darunter geschlossen *jeder* mit harten Zahlen. Die Engine-Auswahl steht damit auf
   zwei Ebenen ungemessener Annahmen (siehe [Beweislage](#beweislage-was-wirklich-belegt-ist)).
2. **Copyleft ist das härtere Ausschlusskriterium als CPU.** Surge XT, Vital und der
   MicroDexed-Wrapper fallen über GPL3 raus, nicht über Rechenlast.
3. **Günstigster erster Schritt: DaisySP PhysicalModeling.** Null Portierungsaufwand, MIT,
   schließt drei Klanglücken auf einmal.

---

## Ausgangslage — was spotymod heute hat

Alles DSP liegt in `Documents/AI/Spotykach` (spotymod), nicht im Residency-Repo.

| Engine | Ort | Typ |
|---|---|---|
| `SynthEngine` (Boot-Default) | `engine/synth/synth_engine.{h,cpp}` | 4-stimmig subtraktiv |
| `TestToneEngine` | `engine/parts/test_tone_engine.h` | Platzhalter für A/B |
| `SamplerEngine` | **nicht geschrieben**, Spec M5 | granular (geplant) |

Pro Stimme (`engine/synth/voice.h`): 2 × `MorphOsc` + Sub-Sine → `daisysp::Svf` LP → Env → Pan.
`MorphOsc` ist **kein Wavetable** trotz Spec-Wortlaut — ein Phasor, analytisch morphend
Sine→Tri→Saw→Pulse mit polyBLEP auf den Unstetigkeiten.

**Klangliche Lücken:** kein FM, kein echtes Wavetable, kein Physical Modeling, keine
Formant-/Vokalsynthese, kein Additive, keine Chords-/Speech-/Particle-Modelle.
Granular schließt sich mit M5 ohnehin ohne Fremdcode.

### Zwei Constraints, die härter filtern als erwartet

**CPU-Budget rechnerisch halb weg — und nie gemessen.** Spec-Schätzung ~50–60 % Worst Case
(8 Stimmen 15–18 %, Part-FX 8–10 %, Reverb 10 %, Mod-Lanes 4–6 %). Der Firmware-Shell-Spec
revidiert den Reverb nach oben auf realistisch 15–25 %, weil er in SDRAM liegen muss und die
Delay-Line-Reads am Cache vorbeigehen. README: *"Not yet tested on real hardware."* M6 ungeplant.
Es existiert **kein einziger gemessener Wert**.

**Lizenz.** In M4.5 wurde bewusst alle DaisySP-LGPL-Linkage entfernt (ReverbSc, PitchShifter),
damit das ausgelieferte Binary keine LGPL-Pflichten trägt. Aktuelle DaisySP-Nutzung ist MIT-only
über das `daisysp_min`-Target.

**Bereits etabliertes Port-Muster:** `third_party/oliverb/` (Mutable-Code + `stmlib_shim.h` —
getrimmte stmlib-Utilities statt Vollabhängigkeit). Genau das bräuchte ein Plaits-Port.

---

## Kandidatenliste

### 1. DaisySP PhysicalModeling + Plaits-Oszillatorkerne ⭐

**Aufwand:** null — DaisySP wird über `daisysp_min` bereits gelinkt. **Lizenz:** MIT
(Copyright Electrosmith + Émilie Gillet), stmlib-frei (Header inkludieren nur `<stdint.h>`).

| Klasse | Klanggewinn |
|---|---|
| `ModalVoice` | Modalsynthese, Mallet-Exciter → click → LPF → Resonator |
| `StringVoice` | Extended Karplus-Strong, "with all the niceties from Rings" |
| `Resonator`, `String` | Resonanzkörper / Kammfilter-Saite |
| `FormantOscillator`, `VosimOscillator` | **Formant-/Vokal-Charakter** — größte Lücke |
| `HarmonicOscillator` | additiv |
| `GrainletOscillator`, `ZOscillator` | Grainlet / Phase-Distortion |
| `VariableSawOscillator`, `VariableShapeOscillator` | erweiterte Analog-Formen |

Provenienz steht im Quelltext: `zoscillator.h` — *"Ported from
pichenettes/eurorack/plaits/dsp/oscillator/z_oscillator.h … written by Emilie Gillet in 2016"*.
Plaits-Klang ohne den Plaits-Baum.

**Einschränkung (wichtig):** Ports und Vereinfachungen, **keine Feature-Parität**. Geliefert
werden Oszillator-Kerne und Einzelstimmen — *nicht* Plaits' Engine-Layer (LPG, interne
Decay-Hüllkurven, Parameter-Morphing, out/aux-Mix) und nicht die höherstufigen Wavetable-,
Chord-, Speech-, Modal- und Percussion-Engines. `Resonator` ist der reduzierte Plaits-interne
Modalkörper, **nicht Elements**. `StringVoice` ist *eine* Saite, nicht Rings' polyphone
Multi-String-Engine, stammt über Plaits von Rings ab (nicht direkt aus Rings), und hat keinen
Bogen-Modellierer — nur `SetSustain(bool)` als kontinuierliche Rausch-Anregung.

**Architektur-Fit:** am besten von allen. Per-Voice-Objekte, passen direkt hinter `IPartEngine`
in die bestehende Voice-Kette.

### 2. Torus / Rings — auf genau dieser MCU bewiesen

`electro-smith/DaisyExamples/patch/Torus`, Portierung Ben Sergentanis nach Émilie Gillet, Target
Daisy Patch. **Vollständiger** Modellsatz: Modal, Sympathetic Strings, Inharmonic Strings,
FM Voice, Western Chords, String-and-Reverb — keine reduzierte Teilmenge.

Kein Stub: `torus.cpp` (11,8 kB), `cv_scaler.cc`, `resources.cpp` (18,75 kB Original-LUTs), plus
`dsp/`-Baum mit `resonator.cc`, `string.cc`, `fm_voice.cc`, `part.cc`, `string_synth_part.cc`,
`strummer.h`, `plucker.h`, `onset_detector.h`, `limiter.h`, `fx/`. Enthält laut Electro-Smith die
Easter-Egg-Features des Originals. Nicht enthalten: der separate "Disastrous Peace"-Modus.
Community-Variante für `patch.Init()` existiert.

**Fit-Problem:** Rings ist konzeptionell ein *Resonator mit Anregung*, kein 4-stimmiger
Poly-Voice-Block. Passt eher als alternativer Part-Modus oder anregbare FX-Stufe denn als Drop-in
in `SynthEngine::kVoices = 4`. Die Behauptung "4 Rings-Stimmen pro Seed" wurde **0-3 widerlegt** —
Polyphonie offen.

### 3. Plaits vollständig (Monorepo + stmlib vendorn)

**Größte Klangbreite.** Klarstellung zur Engine-Zahl: Das offizielle Handbuch dokumentiert
**16 Modelle** in zwei Bänken à 8 (*"Each button cycles through a bank of 8 models. The second
bank is focused on noisy and percussive sounds."*) — VA, Waveshaping, 2-Op-FM, Formant/PD,
Harmonic/Additive, Wavetable, Chords, Speech, dann Granular, Noise, Particle, Modal, String +
3 Drum-Modelle. Die **8 zusätzlichen Engines stammen aus Firmware 1.2** (u.a. 6-Op-FM,
Wave-Terrain) und stehen auf einer separaten Firmware-Seite. 16 + 8 = 24. Beide Gruppen müssen
getrennt bewertet werden.

**Portierung:** Plaits ist **kein eigenständiges Repo**, sondern das Verzeichnis `plaits/` im
Monorepo `pichenettes/eurorack` — und **nicht selbsttragend**:

- `plaits/dsp/engine/engine.h` → `stmlib/dsp/units.h`, `stmlib/utils/buffer_allocator.h`
- `plaits/dsp/voice.h` → `stmlib/stmlib.h`, `stmlib/dsp/filter.h`, `stmlib/dsp/limiter.h`

stmlib ist ein echtes Git-Submodul, nicht in-tree vendored. Kompilieren des `plaits`-Verzeichnisses
allein scheitert bereits im Preprocessing. **CMSIS wird nicht gebraucht** (nur für
STM32F-Firmware-Builds, nicht für den reinen C++-DSP-Teil).

**Reales Risiko — Größe, nicht nur CPU:** Ein Praktiker musste beim patch.init()-Port Chord- und
Wavetable-Engine deaktivieren, weil sie nicht in den Flash passten. Die zugehörige
"nur 128 kB nutzbar"-Behauptung wurde zwar 0-3 widerlegt, aber spotymod hat ein strukturell
gleiches Problem: der Build muss in dieselbe **256 KB SRAM_EXEC**-Region wie die Stock-Firmware,
geprüft per `arm-none-eabi-size` gegen `alt_sram.lds`. Plaits' `resources`-Tabellen zählen dagegen.

**Architektur-Spannung:** Plaits' `Voice` bringt eigene LPG-, Decay- und Morphing-Logik mit, die
mit der bestehenden Env-/Svf-/Pan-Kette kollidiert. Zu entscheiden wäre: geht eine Plaits-Engine
*in* die Voice oder *statt* ihr?

### 4. msfa / Synth_Dexed — 6-Operator-FM

Klanglich **komplementär zu allem MI-Material**; schließt nach Formant die zweitgrößte Lücke.

**Machbarkeit belegt:** MicroDexed läuft auf Teensy 3.6 (Cortex-M4F @ **180 MHz**) und 4.x;
MicroDexed-touch macht 2×16 = **32 gleichzeitige 6-Op-Stimmen** auf einem Teensy 4.1 (M7 @600 MHz).
Bei 8 Stimmen auf 480 MHz also mit Sicherheitsmarge. Bekannte Obergrenze dort: XRUNs bei
`MAX_NOTES=128`, "garbled above about 50-60 notes" auf Teensy 4.0.

**Lizenz-Trennlinie verläuft dateiweise — unbedingt headerweise prüfen:**

- **Apache 2.0** (nutzbar): msfa-Kern, u.a. `dx7note.cpp`, `fm_core.cpp`
- **GPLv3** (nicht nutzbar): MicroDexed-Wrapper, u.a. `dexed.h` (Copyright H. Wirtz 2018–2021)

Upstream-Statement: *"MicroDexed is licensed on the GPL v3. The msfa component … stays on the
Apache 2.0 license to able to collaborate between projects."* `Synth_Dexed` existiert als
ausgefaktorte Bibliothek mit `setMaxNotes()`-Polyphonie-API.

**Aufwand:** kein Daisy/H750-Port bekannt; Teensy-Audio-Library-Abhängigkeiten müssen raus.
Zusätzlich zu Apache 2.0: Attribution, NOTICE-Erhalt, Patent-Termination-Klausel.

### 5. Weitere MI-Module — nicht verifiziert

Clouds, Elements, Warps, Tides, Braids wären die naheliegende MIT-Erweiterung derselben Toolchain,
**aber in dieser Recherche überlebte dazu kein einziger Claim.** Das ist Nichtwissen, kein
negativer Befund.

⚠️ **Braids gesondert prüfen:** Die Repo-README-Regel lautet *"Code (AVR projects): GPL3.0.
Code (STM32F projects): MIT"*. Die Zuordnung von Braids wurde **nicht** verifiziert.

### Ausgeschlossen

**Surge XT** — GPL3 auf dem gesamten Baum inkl. `src/common/dsp`. Keine Linking-Exception, kein
LGPL-Fallback, kein Hardware-/Embedded-Carve-out, kein Dual-Licensing. Firmware ausliefern ist ein
**Conveying-Event** nach GPL3 §5/§6 → Corresponding-Source-Pflicht; bei einem User Product zusätzlich
§6 Anti-Tivoization (Installation Information — Nutzer müssen modifizierte Firmware flashen können).
Schmaler permissiver Pfad nur dateiweise in `sst-basic-blocks`: *"a small number of individual files
… are also available to use in an MIT license context. Those header files are explicitly marked"* —
Default und Masse, insbesondere die Oszillator-Engines, bleiben GPL3.

Gleiches Muster: **Vital** (GPL3), **Dexed-Wrapper** (GPL3). Konsistent mit der M4.5-Entscheidung.

### MIT ist nicht pflichtenfrei

Copyright- und Permission-Notice müssen ausgeliefert werden (About-Screen, Handbuch oder
Lizenzdatei): *"The above copyright notice and this permission notice shall be included in all
copies."* **Nicht mitlizenziert:** Markenname "Mutable Instruments"/"Plaits" und die
CC-BY-SA-3.0-Panelgrafik. MI empfiehlt ausdrücklich, Modulnamen nicht beizubehalten.

Kommerzielle Präzedenzfälle ohne Beanstandung: Arturia MicroFreak/MiniFreak, Behringer Brains
(15 Plaits-Algorithmen), Michigan Synth Works Xena, Poly Effects Beebo.

MI ist seit 2022 abgewickelt, das Repo eingefroren, die MIT-Gewährung unwiderruflich.

---

## Beweislage: was wirklich belegt ist

Methodik: 5 Suchwinkel → 21 Quellen → 90 Claims → 25 adversarial verifiziert (2/3 Refutes killen
einen Claim). **15 bestätigt, 10 widerlegt.**

### Widerlegt — nicht als Beleg verwenden

| Behauptung | Votum |
|---|---|
| Oopsy-Messungen: Wavetable 45 %, Gigaverb 20 %, Pulsar 32 %, Dattoro 10 %, FM 8 % @480 MHz | 1-2 ✗ |
| Fast-Math bringt ~2,4× (Gigaverb 48 % → 20 %) | 0-3 ✗ |
| Plaits-Polyphonie: 3 Stimmen inharmonic string / 1 Stimme modal @24 Partials | 0-3 ✗ |
| "Torus schafft 4 Rings-Stimmen pro Seed" | 0-3 ✗ |
| Daisy Seed hat nur 128 kB nutzbaren Programm-Flash, ~50 kB libDaisy-Overhead | 0-3 ✗ |
| `PlaitsPatchInit`: alle Engines via patch.init(), 16 kB BufferAllocator-Scratch, minimale stmlib-Oberfläche | 0-3 ✗ (3 separate Claims) |
| `mi-plaits-dsp-rs` sei stmlib-frei und `no_std` | 0-3 ✗ (ist ohnehin ein Rust-Port — falsche Referenz für C++) |

**Konsequenz: Per-Engine-Kosten müssen selbst gemessen werden, bevor die 2×4-Architektur
festgeschrieben wird.** Die 8-Stimmen-Machbarkeit ruht derzeit ausschließlich auf Analogien
(Torus läuft; Teensy-4-Dexed schafft 32 Stimmen), nicht auf Messungen.

### Bestätigt, aber schwach

**SRAM vs. SDRAM** (2-1, eine einzige Quelle, NIME-Paper): *"algorithms using SRAM for runtime
memory resulted in better CPU performance than those using SDRAM, sometimes with significant
differences"*, und *"only very few patchers require slower SDRAM space at all"*. Qualitativ, keine
Messzahlen; Geltungsbereich streng genommen nur gen~/Oopsy-Patcher, nicht beliebiger C++-Code.
Quellseite lieferte HTTP 403, Text über Suchindex rekonstruiert. Plaits-Wavetables, granulare
Puffer und Sample-Playback brauchen sehr wohl SDRAM.

**SDRAM-Fallstrick (aus Fetch, nicht in die Top-25-Verifikation gelangt):** `DSY_SDRAM_BSS`-Puffer
sind laufzeit-nullinitialisiert — SDRAM kann **keine compile-time-initialisierten Daten** halten.
Wavetables müssen beim Start generiert oder aus Flash/QSPI kopiert werden. Relevant für jeden Port,
der `const`-Tabellen erwartet.

### Nicht abgedeckt

Zu **Braids, Elements, Warps, Tides, Clouds, Vital, OB-Xd, ChowDSP, STK, Faust physmodels,
Formant-/Vokalsynthese sowie granularen und samplebasierten Engines** überlebte kein verifizierter
Claim. Faust-physmodels und STK erschienen als Quellen, lieferten aber keine bestätigten Aussagen.
Lohnt ggf. eine zweite Runde gezielt auf "Lücken im MI-Klangraum".

---

## Offene Fragen

1. Was kostet eine Plaits-Stimme tatsächlich auf dem H750 @480 MHz — pro Engine, bei 48 kHz und
   Blockgröße 96? **Teilweise beantwortet, nicht geschlossen:** die Bench-Firmware
   (`spotymod`, `docs/bench/2026-07-18-256da41.md`) misst die neun DaisySP-Kandidaten aus
   Kandidat 1, nicht die vollen Plaits-Engines — der Plaits-Monorepo-Port ist für die Bench
   explizit out of scope, die Frage verengt sich also statt sich zu schließen. Als Vielfache
   einer echten spotymod-Stimme: `modal_voice` 6.1x und `resonator` 5.5x sind die einzigen
   echten Ausreißer, `string_voice` 1.9x, `vosim_osc` 0.92x und `z_osc` 0.99x liegen etwa bei
   Parität, `formant_osc` 0.29x, `harmonic_osc` 0.48x, `grainlet_osc` 0.66x und
   `variable_shape_osc` 0.19x sind günstiger als eine reale Stimme — vier von neun Kandidaten.
   Ungemessen bleibt die Plaits-Engine-Schicht selbst (LPG, Decay-Hüllkurven,
   Parameter-Morphing), die auf diesen Kernen aufsetzt.
2. Welche Plaits-Engines brauchen wirklich SDRAM statt SRAM (Wavetable, Speech, Particle, Granular,
   Wave-Terrain) — und wie viel Flash belegen die `resources`-Tabellen? **Kosten-Hälfte
   beantwortet:** der Grain-Read-Proxy (8 verstreute interpolierte Stereo-Reads pro Sample,
   identisches Muster in beiden Regionen, 64-KB-Fenster) kostet 5.3x in SDRAM gegen SRAM; das
   Oliverb-SRAM/SDRAM-Paar liegt bei 1.1x. **Welche-Engines- und Flash-Hälfte bleiben offen** —
   die Bench sagt nichts darüber, welche Plaits-Engines überhaupt SDRAM brauchen oder wie groß
   ihre `resources`-Tabellen sind.
3. Sind Braids, Elements, Warps, Tides, Clouds ebenfalls MIT (STM32F-Bucket) oder fällt einzelnes in
   den GPL3-AVR-Bucket? Existieren dafür Daisy-Ports analog zu Torus?
4. Wie groß ist der Aufwand, den Apache-2.0-msfa-Kern von den Teensy-Audio-Abhängigkeiten zu lösen,
   und wie viele 6-Op-Stimmen bleiben bei 480 statt 600 MHz?
5. Geht eine Plaits-Engine *in* die bestehende Voice oder *statt* ihr? (LPG-/Decay-Kollision)

---

## Vorgeschlagene Reihenfolge

**Gemessen, nicht mehr geschätzt.** Die Benchmark-Firmware ist gebaut und gelaufen:
`spotymod`s `docs/bench/2026-07-18-256da41.md` schaltet Engine für Engine auf echter Hardware
durch (Daisy Seed, 480 MHz, 48 kHz, Blockgröße 96). Ergebnis: das 2×4-Budget passt **nicht** —
die volle Instrument-Worst-Case liegt bei 165 % (offline) bzw. 164 % (im echten Audio-Callback
verankert) des Blockbudgets, und die Modulationsebene allein kostet ~33 % statt der im
Design-Spec geschätzten 4–6 %. Von den neun DaisySP-Kandidaten sind vier günstiger als eine
reale spotymod-Stimme; nur `modal_voice` (6.1x) und `resonator` (5.5x) sind echte Ausreißer
(siehe Offene Frage 1 oben). Die Priorisierung unten bleibt richtig, muss jetzt aber gegen ein
Budget geplant werden, das zuerst Stimmen oder FX abbauen muss, bevor neue Engines dazukommen.

Priorisierung unverändert:

1. **DaisySP PhysicalModeling** — null Aufwand, deckt Modal + Karplus-Strong + Formant/Vosim ab,
   also drei Lücken auf einen Schlag
2. **FM aus dem msfa-Kern** (Apache 2.0, dateiweise prüfen)
3. **Voller Plaits-Baum** bei Bedarf — größter Klanggewinn, aber Flash-/Architekturrisiko

`SamplerEngine` (M5) schließt die granulare Lücke ohnehin ohne Fremdcode.

---

## Quellen

- https://pichenettes.github.io/mutable-instruments-documentation/modules/plaits/open_source/
- https://pichenettes.github.io/mutable-instruments-documentation/modules/plaits/manual/
- https://pichenettes.github.io/mutable-instruments-documentation/modules/plaits/firmware/
- https://pichenettes.github.io/mutable-instruments-documentation/modules/rings/secrets/
- https://github.com/pichenettes/eurorack
- https://github.com/electro-smith/DaisyExamples/tree/master/patch/Torus
- https://electro-smith.github.io/DaisySP/annotated.html
- https://github.com/electro-smith/DaisySP/tree/master/Source/PhysicalModeling
- https://electro-smith.github.io/libDaisy/md_doc_2md_2__a6___getting-_started-_external-_s_d_r_a_m.html
- https://codeberg.org/dcoredump/MicroDexed
- https://codeberg.org/dcoredump/Synth_Dexed
- https://github.com/google/music-synthesizer-for-android
- https://surge-synthesizer.github.io/faq/
- https://github.com/surge-synthesizer/sst-basic-blocks
- https://nime.pubpub.org/pub/0u3ruj23 (SRAM/SDRAM, 2-1, HTTP 403)
