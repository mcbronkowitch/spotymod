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

// --- the cloud ---
constexpr int    kGrains        = 8;         // per part
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
constexpr float  kSizeCeilS     = 42.f;

// Minimum samples between grain spawns, at any SIZE and any kOverlap.
//
// This is a CPU guard and it belongs on the interval, not on grain length:
// _spawn_every = _grain_len / kOverlap, so a length floor stops bounding the
// spawn rate the moment kOverlap rises. M5a floored length at 64 samples,
// which with kOverlap = 16 would have permitted a spawn every 2 samples.
// 8 samples caps the rate at 6 kHz per part. NOT ear-tunable.
constexpr float  kSpawnMinSamples = 8.f;

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
constexpr float  kScatterOctProb  = 0.25f;   // chance a chord note jumps an octave

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
