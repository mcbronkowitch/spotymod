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
constexpr float  kDefaultFeedback = 0.95f;   // knob position; -3 dB on the -60..0 dB curve

// --- the cloud ---
constexpr int    kGrains        = 8;         // per part
constexpr int    kCtrlInterval  = 96;        // must equal SynthEngine::kCtrlInterval

// SIZE: exponential 20 ms .. 2 s, size_s = kSizeMinS * kSizeRange^n
constexpr float  kSizeMinS      = 0.02f;
constexpr float  kSizeRange     = 100.f;

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
constexpr float  kScatterPosFrac  = 0.25f;   // +-1/4 of content length
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
constexpr float  kDetuneCeilCt  = 35.f;      // DTUN spread ceiling, matches the synth
constexpr float  kSubMaxShare   = 1.f;       // SUB 1 = every grain an octave down

}  // namespace sampler_cfg
}  // namespace spky
