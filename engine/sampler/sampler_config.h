#pragma once
#include <cstddef>

// Tuning constants for the M5 texture deck (spec 2026-07-18
// sampler-texture-deck-design.md). Values marked "ear-tunable" are taste,
// not contract -- changing them changes the sound, not the correctness.
namespace spky {
namespace sampler_cfg {

// --- record core (carried over from src/core/config.h:57,73) ---
// 192 samples == 4 ms @ 48 kHz, and == the hann table size in fx_util.h, so
// the fade counter indexes the curve 1:1. TRAGEND ist die zweite Gleichung:
// die Tabellengroesse. Die 4 ms gelten nur bei 48 kHz -- dies ist eine
// Sample-Zahl, waehrend _cut (SoftSwitch) ueber init(sample_rate) echte 4 ms
// haelt, sodass die beiden bei anderen Raten auseinanderlaufen.
constexpr size_t kRecordFade      = 192;
// Knob position. NOTE: this sits ABOVE kFbKnee, so it means something
// slightly different than it did in M5a -- about -1.8 dB (linear ~0.817)
// rather than -3 dB, but ONLY once something calls set_feedback(0.95) and
// runs it through the post-knee curve above. SampleBuffer::init() does not:
// it computes _feedback directly with the pre-knee formula
// (60*(kDefaultFeedback - 1) dB, i.e. -3 dB / ~0.708 linear) and never calls
// set_feedback() itself, so the actual boot state -- before any host pushes
// the knob -- is still the M5a -3 dB, unchanged. -1.8 dB / 0.817 is what the
// knob reads once a host (or a test, see F-06 below) explicitly sets it to
// this position; it is not what plays on power-up.
constexpr float  kDefaultFeedback = 0.95f;

// Record-feedback knee. Below this the mapping is the M5a one exactly:
// knob n gives -60 + 60*n dB, so 0.9 is -6 dB. Above it, travel runs from
// -6 dB on to kFbMaxDb, and the buffer saturates into itself.
//
// Unity is consequently NOT at the top of travel but at knob ~0.971 -- a
// narrow but findable spot where the loop sustains forever. Ear-tunable:
// lowering kFbMaxDb widens that spot, raising it narrows it.
constexpr float  kFbKnee   = 0.9f;
constexpr float  kFbMaxDb  = 2.5f;

// Ab welchem KOEFFIZIENTEN (nicht Knopfwert) der Overdub in fast_tanh
// laeuft. Frueher war das implizit 1.0, und genau dort lag der Fehler: der
// Overdub ist ein Integrator mit Fixpunkt in/(1-fb), der knapp UNTER Unity
// unbegrenzt waechst, waehrend er ueber Unity von tanh gefangen wird. Nach
// 60 s Overdub eines 0.5-Signals gemessen: Knob 0.9700 -> Peak 87,
// 0.9705 -> 234 (asymptotisch ~579), 0.9710 -> 2.3, 1.0 -> 2.31. Das
// lauteste Verhalten des Geraets lag damit in einem ~0.001 breiten Fenster
// unterhalb des Anschlags.
//
// Der Wert ist gemessen, nicht geraten. Peak nach 30 s Overdub eines
// 0.5-Signals, alle vier Varianten auf demselben Raster (0.94 .. 0.97 in
// Schritten von 0.0025, darueber 0.0005, weil die Spitze schmal ist):
//
//              Default (0.95)   hoechste Spitze     Anschlag 1.0   Inversion
//   ohne       2.74             132.2 @ 0.9705      1.756          75x
//   0.98       2.74              16.79 @ 0.9675     1.756          9.6x
//   0.90       2.74               4.16 @ 0.9575     1.756          2.4x
//   unbedingt  1.18             keine               1.756          1.0x
//
// Der Anschlag ist in allen vier Zeilen dieselbe Zahl, und das ist kein
// Messfehler: bei Knopf 1.0 ist der Koeffizient 10^(2.5/20) = 1.33 und
// liegt damit ueber JEDER der Schwellen, also laeuft dort ueberall
// derselbe Code. Eine frueherer Fassung dieser Tabelle trug hier 2.31 fuer
// "ohne" -- eine 60-s-Messung, die versehentlich neben die 30-s-Spalten
// geraten war. Wer die Tabelle erweitert: dieselbe Dauer, dasselbe Raster.
//
// 0.90 gewinnt gegen 0.98 ohne Gegenleistung: gleiche Bauart, halb so hohe
// Spitze, und der Auslieferungs-Default bleibt bei beiden unberuehrt --
// kDefaultFeedback = 0.95 bildet ueber die kFbKnee-Kennlinie (set_feedback())
// auf ~0.817 ab und damit unter jede der beiden Schwellen. Das gilt fuer den
// Wert, den ein Host ueber set_feedback(0.95) tatsaechlich einstellt (und
// den der F-06-Test unten so nachstellt) -- der reine Boot-Zustand VOR jedem
// set_feedback()-Aufruf ist etwas anderes: SampleBuffer::init() setzt
// _feedback direkt ueber die Vor-Knie-Formel und landet bei -3 dB / ~0.708
// (siehe kDefaultFeedback oben). Gefaerbt wird in beiden Faellen nur der
// oberste Zipfel des Knopfwegs (ab ~0.96), also genau die Zone, die der
// Kommentar bei kFbKnee ohnehin als Selbstsaettigung beschreibt.
//
// Ehrlich bleibt: eine Restunstetigkeit an der Schwelle. Direkt darunter
// steht der unsaturierte Fixpunkt in/(1-fb) = 5, direkt darueber faengt
// tanh bei ~1.3 -- ein Sprung um Faktor 3.2, gegen Faktor 75 vorher. Ganz
// verschwindet sie nur mit unbedingtem tanh, und das kostet den Default
// 57 % seines Pegels. Das ist eine Hoerentscheidung und keine technische,
// deshalb steht sie hier als Option und nicht als Code.
//
// Ear-tunable, aber nach oben gebunden: ueber ~0.95 waechst die Spitze
// wieder schnell (bei 0.98 schon auf 16.8), unter ~0.8 beginnt die Faerbung
// in gehoerte Einstellungen zu greifen.
constexpr float  kFbSatKnee = 0.90f;

// --- the cloud ---
constexpr int    kGrains        = 16;        // per part
constexpr int    kCtrlInterval  = 96;        // must equal SynthEngine::kCtrlInterval

// SIZE: piecewise exponential, 1 ms .. 42 s.
//
// The middle segment [kSizeKneeLo, kSizeKneeHi] is the M5a curve unchanged,
// kSizeMinS * kSizeRange^n -- every setting that has been listened to stays
// at the knob position where it was. The two outer fifths steepen into new
// territory. Continuous in value at both knees, deliberately not smooth in
// slope; the kinks are audible as a change of pace, which is the point.
constexpr float  kSizeMinS      = 0.02f;
constexpr float  kSizeRange     = 100.f;
constexpr float  kSizeKneeLo    = 0.2f;
constexpr float  kSizeKneeHi    = 0.8f;
constexpr float  kSizeFloorS    = 0.001f;   // 1 ms: a pitched buzz, not a texture
// 42 s == the record buffer's capacity at 48 kHz, so at the top of travel a
// grain reads the entire loop exactly once under a single window. Beyond
// this the modulo fold in read_linear would only repeat material the same
// grain already covered.
//
// Die Rechtfertigung ist ratenspezifisch, die Konstante aber eine DAUER und
// die Kapazitaet folgt ihr: beide Hosts allozieren sekundenbasiert
// (host/render/main.cpp:23,48 und host/vcv/src/Spotymod.cpp:105,304 rechnen
// 42.0 * sample_rate). Der Puffer fasst damit bei jeder Rate 42 s, und das
// "genau einmal unter einem Fenster"-Argument oben bleibt gueltig.
//
// (Eine frueherer Fassung dieses Absatzes behauptete das Gegenteil -- bei
// 96 kHz reiche die Kapazitaet nur fuer die Haelfte. Das war falsch und lud
// dazu ein, eine gesunde Konstante zu reparieren.)
//
// Was bei 96 kHz tatsaechlich auseinanderlaeuft, steht bei kRecordFade: das
// ist eine Sample-Zahl (4 ms bei 48 kHz, 2 ms bei 96 kHz), waehrend _cut
// (SoftSwitch) ueber init(sample_rate) echte 4 ms haelt.
constexpr float  kSizeCeilS     = 42.f;

// Pitch: piecewise, unity at 0.5. The middle half of travel [0.25, 0.75] is
// the M5a mapping 8^(n-0.5) exactly -- +-9 semitones there, unchanged. Both
// outer quarters steepen to reach +-kPitchOctaves at the ends.
constexpr float  kPitchKneeLo  = 0.25f;
constexpr float  kPitchKneeHi  = 0.75f;
constexpr float  kPitchOctaves = 4.f;

// Minimum samples between grain spawns, at any SIZE and any kOverlap.
//
// This is a CPU guard and it belongs on the interval, not on grain length:
// _spawn_every = _grain_len / kOverlap, so a length floor stops bounding the
// spawn rate the moment kOverlap rises. M5a floored length at 64 samples,
// which with kOverlap = 16 would have permitted a spawn every 2 samples.
// 8 samples caps the rate at 6 kHz per part. NOT ear-tunable.
constexpr float  kSpawnMinSamples = 8.f;

// Wieviele Grains ueber den angeforderten Overlap hinaus gleichzeitig klingen
// duerfen. Wie kSpawnMinSamples ein CPU-Schutz und NICHT ear-tunable: er
// deckelt die Kosten des schlechtesten Blocks, nicht den Klang des mittleren.
//
// Der Grund, gemessen. _spawn_every ist _grain_len / _overlap, aber das
// tatsaechliche Intervall ist _spawn_every * (1 + _spawn_jitter), und der
// Jitter ist bei MOTION 1 +-kScatterTimeFrac = +-75 %. Die Grainlaenge bleibt
// dabei fest, also stapeln kurze Intervalle Grains auf: die Zahl der
// gleichzeitig klingenden Grains schwankt bei DENS 8 zwischen 5 und 11 statt
// bei 8 zu stehen. Exakt gezaehlt (Grain::process-Aufrufe pro 96-Sample-Block,
// nicht gestoppt) ueber 1000 Bloecke bei SIZE 0.05, DENS max:
//
//   MOTION 1: Mittel 732 Grain-Samples/Block, Maximum 998 -> Faktor 1.36
//   MOTION 0: Mittel 757,                     Maximum 762 -> Faktor 1.01
//
// Der Jitter allein ist also der ganze Ausschlag. Auf der Hardware kostet das
// dieselben 1.30x Spitze-zu-Mittel in JEDER Sampler-Zeile des Bench, auch in
// sampler_win_sram, wo der ganze Record-Puffer 64 KB im SRAM liegt -- es ist
// die Grainzahl, kein Speichereffekt. Im ganzen Instrument mit beiden Parts
// auf dem Sampler wurde daraus 92 % Budget im Mittel und 117 % im schlimmsten
// Block (docs/bench/2026-07-22).
//
// Warum ein Deckel und nicht ein kleinerer kScatterTimeFrac: der Jitter ist
// ear-tunable und gehoert dem Ohr, der Blockdeckel ist eine
// Echtzeitschranke. Ein kleinerer Jitter macht den Ausschlag nur
// unwahrscheinlicher, ein Deckel macht ihn unmoeglich -- und fuer einen
// Audio-Callback ist die Schranke der Punkt, nicht der Erwartungswert.
//
// Was er kostet: in den dichtesten Momenten faellt ein Spawn aus, gezaehlt in
// dropped_spawns(). Bei DENS 8 betrifft das den Anteil der Zeit, in dem
// ueberhaupt mehr als kOverlapMax + kSpawnHeadroom Grains klingen wuerden.
//
// Der WERT ist eine Hoerentscheidung und keine Messung, weil er zwei Dinge
// gleichzeitig stellt. Der Deckel bestimmt naemlich auch die erreichbare
// Poolgroesse, und len_ceil (sampler_engine.cpp) rechnet gegen dieselbe Zahl
// -- er deckelt damit, wie weit Tape ein Grain strecken darf:
//
//   Headroom   Grains   Tape-Decke bei DENS max   Solo-Spitze/Mittel (HW)
//   1          9        1.125x  (~2 Halbtoene)    1.14
//   2          10       1.25x   (~4 Halbtoene)    1.18   <- gewaehlt
//   3          11       1.375x  (~5 Halbtoene)    -
//   ohne       16       2x      (1 Oktave)        1.33
//
// Bastian, 2026-07-22, nach Vorlage der Tabelle: "mach 2". Die Begruendung
// gehoert dem Ohr -- Tape bei hoher Dichte war ihm die vier Zehntel
// Spitzenreduktion wert. Wer hier spaeter aufraeumen will: 1 ist NICHT das
// bessere Default, es ist die andere Seite desselben Handels.
//
// Am Instrument-Peak aendert die Wahl fast nichts (113.7 % bei 1 gegen
// 113.1 % bei 2, docs/bench/2026-07-22): was dort ueber dem Budget steht,
// ist nicht die Grainzahl, sondern die Dauerlast zweier Wolken unter der
// vollen FX-Kette.
constexpr int    kSpawnHeadroom = 2;

// Harte Obergrenze fuer die Grain-Laenge in Ausgangssamples.
//
// Grain haelt die Leseposition als Startframe plus relativen Offset, und
// dieser Offset friert ein, sobald die lokale float32-ulp die Schrittweite
// _ratio erreicht (grain.h). Ab _len >= 2^23 = 8 388 608 ist das fuer die
// erreichbaren Ratios der Fall, und das Grain gibt fuer den Rest seines
// Fensters DC aus.
//
// grain.h hielt das fuer unerreichbar und rechnete dafuer mit der
// Pool-Decke bei kOverlap = 8 (kGrains * _spawn_every = 4 032 000). Seit
// DENS den Overlap zur Laufzeit auf 1 stellen kann, ist dieselbe Decke
// _grain_len * kGrains / 1 = 32 256 000 -- viermal ueber der Schranke.
// Erreichbar mit ENG Sampler, GENE SIZE 1.0, TAPE an, TUNE 0, DENS 0.
//
// 2^22 laesst eine Oktave Reserve zur Stall-Schranke. NICHT ear-tunable:
// das ist eine Float-Grenze, kein Klangwert.
//
// Wo diese Decke wirklich greift, und das ist mehr als die eine Ecke oben:
// sie bindet, sobald len_ceil = _grain_len * kGrains / _overlap sie
// uebersteigt, also bei Overlap unter ~7.7 zusammen mit SIZE ueber ~5.5 s
// und einem Tape-Ratio unter ~0.48. Das ist ein knappes Drittel des
// DENS-Wegs zusammen mit einem guten Drittel des Pitch-Bereichs, nicht nur
// DENS 0. Hoerbar ist es dort trotzdem nicht: 2^22 Samples sind 87 s, weit
// jenseits jedes musikalischen Grains, und gekappt wird nur, was ohnehin
// als Standbild wahrgenommen wird. Aber die Reichweite gehoert benannt,
// statt sie als Extremfall zu verbuchen.
constexpr float  kGrainLenCeil = 4194304.f;   // 2^22

// Overlap range for the DENS knob in the sampler (spec 2026-07-21
// morphagene-controls). The ceiling stays at kGrains / 2 = 8 and does NOT
// rise to kGrains: above 8 the pool-throughput bound
// len_ceil = _spawn_every * kGrains (sampler_engine.cpp) starts trimming
// grain length silently. Downward the bound only gets looser, so lowering
// overlap is safe under all conditions.
constexpr float  kOverlapMin = 1.f;
constexpr float  kOverlapMax = 8.f;

// SCAN: the running playhead (spec 2026-07-21 morphagene-controls). The knob
// is bipolar; the sign is the direction. The curve is piecewise, mirroring
// the SIZE curve's shape:
//   |n| < kScanDead          -> exactly 0. A real dead zone, so a frozen head
//                               stays frozen under knob noise.
//   kScanDead .. kScanKnee   -> exponential, kScanMinRate .. 1.0x realtime.
//   above kScanKnee          -> linear, 1.0x .. kScanMaxRate.
// Realtime (1.0x) therefore lands on a fixed, findable knob position instead
// of somewhere in the sweep. The top quarter carries the factor 8 and is the
// steepest stretch of the curve -- if it plays too nervously, the fix is an
// exponential top segment, not a smaller range (spec "Nicht in diesem
// Entwurf" / listening notes).
constexpr float  kScanDead    = 0.02f;
constexpr float  kScanKnee    = 0.75f;
constexpr float  kScanMinRate = 0.001f;
constexpr float  kScanMaxRate = 8.f;

// SCAN's fixed lag behind the write head while a recording is running
// (spec 2026-07-21 morphagene-controls). Folding the playhead modulo content
// length -- the ordinary SCAN behaviour above -- assumes content is fixed;
// while recording, content grows by one frame per sample, and SCAN at
// realtime forward grows _scan_pos by the identical amount from the same
// zero start, so _scan_pos == content every control tick and the fold resets
// to 0 every time: the head is pinned, not running. The fix is to clamp
// instead of fold while recording (_update_control), holding the read
// position a fixed distance behind the write head rather than exactly on it
// -- exactly on it is the case sampler_engine.cpp's `span = content - 1.f`
// comment (_spawn_one) already documents as making the cloud go near-silent.
// Comfortably above kRecordFade's 4 ms record blend so the lag is never
// swallowed by it; short enough that overdubbing still feels like
// listening just behind the write head, not stale. Ear-tunable.
constexpr float  kScanRecordLagS = 0.25f;

// Grain window: the ATK/DEC halves each span at most this fraction of the
// grain, so a fully-open ATK and DEC still leave the window a real shape
// rather than two ramps meeting at a point.
constexpr float  kWindowHalfMax = 0.5f;
// ...and at least this fraction, so a closed knob is still click-free.
constexpr float  kWindowHalfMin = 0.02f;

// MOTION scatter, at MOTION = 1 (all ear-tunable):
// +-the whole content length. Was 0.25 through M5a, which confined MOTION's
// read position to a quarter of the buffer and is the likeliest reason the
// cloud never reached "diffuse fog" (see the Open section in
// 2026-07-18-sampler-texture-deck-design.md). Ear-tunable.
constexpr float  kScatterPosFrac  = 1.0f;
constexpr float  kScatterTimeFrac = 0.75f;   // spawn-interval jitter, fraction of interval
// kScatterOctProb (0.25) lived here: the chance a grain jumped an octave.
// Removed 2026-07-21 with the scatter itself -- it was the one MOTION scatter
// that moved PITCH, which the sampler must hold still so a sampler deck and a
// synth deck can play in the same key. See SamplerEngine::_next_ratio.

// --- voice row, remapped ---
constexpr float  kCutoffMinHz   = 60.f;      // same rails as the synth FILTER
constexpr float  kCutoffMaxHz   = 14000.f;
// The FILT fade invariant, mirrored from SynthEngine (synth_engine.h:44-48):
// the left half overdrives the rail by exactly the blend zone, so t = -1
// lands in silence at ANY lane position. Invariant: kFiltLeftScale >= 1 + kFiltFadeRange.
constexpr float  kFiltLeftScale = 1.25f;
constexpr float  kFiltFadeRange = 0.25f;
// The cloud's resting cutoff, as a normalized position on the 60 Hz..14 kHz
// rail. The sampler has no FILTER lane -- lane 1 is SIZE (grain length) --
// so FILT trims from this fixed neutral instead of from a lane value.
// Wiring it to LANE_SIZE (as the synth does, where lane 1 IS filter) welded
// grain length to brightness and left the cloud behind a ~916 Hz lowpass at
// the boot base. Ear-tunable: 0.75 -> ~3.6 kHz.
constexpr float  kFiltNeutral = 0.75f;
constexpr float  kDetuneCeilCt  = 35.f;      // DTUN spread ceiling, matches the synth
constexpr float  kSubMaxShare   = 1.f;       // SUB 1 = every grain an octave down

// Overlap-normalization (1/sqrt(active)) smoothing time constant. Ear-tunable,
// but chosen from measurement, not taste: "sampler: FLOW is a standing
// cloud" 's continuity ratio (lowest/highest RMS window) and the max
// sample-to-sample delta over the same 3 s FLOW render, at kFiltNeutral =
// 0.75f, for candidate values against the two references (raw/unsmoothed
// _norm: ratio 0.64, delta 0.073; per-grain latched gain, since reverted:
// ratio 0.56, delta 0.102):
//   2 ms:  ratio 0.693, delta 0.0728
//   5 ms:  ratio 0.725, delta 0.0729
//   10 ms: ratio 0.748, delta 0.0738   <- chosen
//   20 ms: ratio 0.744, delta 0.0767
// 10 ms gives the best ratio of the four while its delta is still within ~1%
// of the unsmoothed reference; 20 ms's ratio drops back below 10 ms's while
// its delta clearly worsens -- the "lags real density changes" failure mode
// starting to show. Also matches _level's own 10 ms constant.
constexpr float  kNormSmoothS   = 0.01f;

// --- slice groove (spec 2026-07-22 sampler-slice-groove-design.md) ---
// Marker capacity. 512 x 8 bytes = 4 KB SRAM. With kOnsetRefractS = 40 ms the
// map covers ~20 s of continuous worst-case onset rate before it is full; a
// full map ignores further onsets (oldest content keeps its markers). NOT
// ear-tunable: it is a memory budget.
constexpr int    kMaxSlices     = 512;
// Below this many markers the engine treats the material as transientless and
// slices on the tempo grid instead (SamplerEngine::_pool_size). Ear-tunable.
constexpr int    kMinSlices     = 4;
// Onset detector: fast/slow envelope pair on the written frame. All five are
// ear-tunable EXCEPT the refractory time's floor role: it also bounds marker
// density and therefore how fast the map fills (see kMaxSlices).
constexpr float  kOnsetFastS    = 0.001f;   // fast envelope time constant
constexpr float  kOnsetSlowS    = 0.080f;   // slow envelope time constant
constexpr float  kOnsetThresh   = 2.0f;     // fast/slow ratio that fires
constexpr float  kOnsetRearm    = 1.2f;     // ratio must fall below to re-arm
constexpr float  kOnsetFloor    = 0.01f;    // absolute fast-env floor (noise gate)
constexpr float  kOnsetRefractS = 0.040f;   // dead time after an onset
constexpr float  kOnsetPreRollS = 0.002f;   // marker sits this far BEFORE detection
// SIZE in STEP: window = slice length x 2^((SIZE - 0.5) * 2 * kSliceSizeOct),
// so knob centre is exactly one slice (findable unity, the SCAN 1.0x idiom),
// the bottom is 1/16th of the slice (attack tip), the top 16x (overrun into
// following material). Ear-tunable.
constexpr float  kSliceSizeOct  = 4.f;

// --- feel accents (spec 2026-07-23 sampler-feel-accents-design.md) ---
// STEP polyphony ceiling, fixed. DENS used to set it (ceil(_overlap) +
// kSpawnHeadroom), which meant turning DENS down to thin the phrase ALSO
// dropped composed notes -- silently, with nothing on the panel to show it.
// 10 == the old ceiling at DENS maximum (kOverlapMax + kSpawnHeadroom), so
// the worst case this was measured at is unchanged; it is an emergency stop,
// not a control. NOT ear-tunable: it is a CPU budget.
constexpr int    kStepGrainCeil = 10;

}  // namespace sampler_cfg
}  // namespace spky
