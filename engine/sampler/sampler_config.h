#pragma once
#include <cstddef>

// Tuning constants for the M5 texture deck (spec 2026-07-18
// sampler-texture-deck-design.md). Values marked "ear-tunable" are taste,
// not contract -- changing them changes the sound, not the correctness.
namespace spky {
namespace sampler_cfg {

// --- record core (carried over from src/core/config.h:57,73) ---
// 192 samples == 4 ms @ 48 kHz, and == the hann table size in fx_util.h, so
// the fade counter indexes the curve 1:1. Both facts are load-bearing.
constexpr size_t kRecordFade      = 192;
// Knob position. NOTE: this sits ABOVE kFbKnee, so it means something
// slightly different than it did in M5a -- about -1.8 dB rather than -3 dB.
// The boot state gets marginally hotter and still stops short of unity.
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
// 0.5-Signals, ueber den Knopfweg 0.88 .. 1.00, fuer drei Kandidaten:
//
//              Default (Knopf 0.95)   hoechste Spitze   Anschlag 1.0
//   ohne       2.74                   234 @ 0.9705      2.31
//   0.98       2.74                   9.40 @ 0.965      1.76
//   0.90       2.74                   3.53 @ 0.955      1.76
//   unbedingt  1.18                   keine             1.76
//
// 0.90 gewinnt gegen 0.98 ohne Gegenleistung: gleiche Bauart, halb so hohe
// Spitze, und der Auslieferungs-Default bleibt bei beiden unberuehrt --
// kDefaultFeedback = 0.95 bildet ueber die kFbKnee-Kennlinie auf ~0.817 ab
// und damit unter jede der beiden Schwellen. Gefaerbt wird nur der oberste
// Zipfel des Knopfwegs (ab ~0.96), also genau die Zone, die der Kommentar
// bei kFbKnee ohnehin als Selbstsaettigung beschreibt.
//
// Ehrlich bleibt: eine Restunstetigkeit an der Schwelle. Direkt darunter
// steht der unsaturierte Fixpunkt in/(1-fb) = 5, direkt darueber faengt
// tanh bei ~1.3 -- ein Sprung um Faktor 2.8, gegen Faktor 101 vorher. Ganz
// verschwindet sie nur mit unbedingtem tanh, und das kostet den Default
// 57 % seines Pegels. Das ist eine Hoerentscheidung und keine technische,
// deshalb steht sie hier als Option und nicht als Code.
//
// Ear-tunable, aber nach oben gebunden: ueber ~0.95 waechst die Spitze
// wieder schnell (bei 0.98 schon auf 9.4), unter ~0.8 beginnt die Faerbung
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
// That justification is RATE-SPECIFIC, and the constant is a duration, not a
// frame count. At 96 kHz this asks for twice the capacity the buffer has, so
// the top of SIZE spans two loops rather than one and the "exactly once
// under a single window" argument no longer holds -- the fold simply repeats
// the first pass. Not a crash and not a range to narrow: read_linear folds
// safely and the result is a slower swell over repeated material, which is
// still musical. Recorded so nobody re-derives the 42 as rate-independent.
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

// STEP burst: grains keep spawning this long past the gate falling, so a
// chopped texture ends with a tail rather than a cut. Ear-tunable.
constexpr float  kBurstReleaseS = 0.06f;

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

}  // namespace sampler_cfg
}  // namespace spky
