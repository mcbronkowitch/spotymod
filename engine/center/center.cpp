#include "center/center.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kQuarter = TWO_PI * 0.25f;   // pi/2, for the equal-power law

constexpr float kOuTau   = 45.f;             // weather time constant (s)
constexpr float kOuSigma = 0.10f;            // weather noise scale

// DRIFT taps (polarity/scale of the full walk — spec table). Index 0 = A, 1 = B.
constexpr float kRateTap[2]  = { 1.0f, -0.6f };   // x kRateOct octaves
constexpr float kShapeTap[2] = { 0.8f, -1.0f };   // x kShapeMax
constexpr float kTuneTap[2]  = { 0.5f, -0.9f };   // x kTuneCents
constexpr float kRateOct   = 0.5f;                // up to +/- 1/2 octave
constexpr float kShapeMax  = 0.15f;               // up to +/- 0.15 shape
constexpr float kTuneCents = 25.f;                // up to +/- 25 cents

constexpr float kK           = 0.15f;   // Kuramoto phase-pull gain (tune by ear)
// COUPLE convergence pulls a FREE bank toward the geometric mean of the two
// base rates (always <= the faster bank, so it can never run away to audio
// rate). The clamp is only a transient safety net, so it must be wide enough
// that full COUPLE can actually reach the mean across the musical rate range;
// the old +/-1 octave (0.5..2.0) truncated convergence and left banks that were
// more than a ~4:1 ratio apart perpetually drifting. +/-5 octaves covers the
// whole practical FREE span (0.02..30 Hz needs up to ~39x; this reaches ~32x).
constexpr float kRateClampLo = 0.03125f;   // 1/32  (-5 octaves)
constexpr float kRateClampHi = 32.0f;      //        (+5 octaves)

// COUPLE fully clockwise (knob == 1) is a HARD lock, not the soft Kuramoto nudge.
// Two things are essential and were both wrong before:
//  1. Lock the RAW pitch phase (the sawtooth that clocks the sequencer / loop), NOT
//     phase_eff. phase_eff adds the EVOLVE offset _ev_phase, which a melody change
//     (MELO != 0) zeroes at the phrase wrap — a step the servo then chased by shoving
//     the raw clock around, i.e. the "drifts when the melody changes" report. The raw
//     phase is continuous through a regen, so a melody change no longer disturbs it.
//  2. Push hard enough to overcome EVOLVE's rate wander. MELO drives _ev_rate, which
//     modulates the raw phase increment by up to +/-20% (lane.cpp: _phase_inc*(1+_ev_rate)).
//     The gentle kK=0.15 nudge can't outrun that, so the loops slip. At full COUPLE we
//     use kKHard with a per-tick slew cap: the cap keeps engage/large errors click-free
//     and the servo stable, while kKHard > the disturbance drives the residual to ~0.
constexpr float kFullCouple = 0.999f;   // knob effectively fully CW -> hard lock
constexpr float kKHard      = 2.0f;     // hard-lock phase-pull gain (>> the _ev_rate wander)
constexpr float kLockCap    = 0.35f;    // max rate correction per tick (> _ev_rate's 0.2)
}

void Center::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _cr = sample_rate / static_cast<float>(kCtrlInterval);
    _transport.init(_cr);

    _morph_target = 0.5f; _morph = 0.5f;
    _morph_smooth.init(_cr, 0.03f);
    _morph_smooth.reset(0.5f);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    _couple = 0.f; _phase_err = 0.f;

    _drift_target = 0.f; _drift = 0.f;
    _drift_smooth.init(_cr, 0.3f);
    _drift_smooth.reset(0.f);
    _ou = 0.f; _weather = 0.f;
    _weather_rng.seed(seed);

    _spot_rng.seed(seed ^ 0x51207AB7u);
    _settle_ctr = 0;
    _settle_coef = std::exp(-1.f / (0.3f * _cr));
}

void Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb) {
    _transport.tick();

    // --- MORPH (equal-power, smoothed at control rate) ---
    _morph = _morph_smooth.process(_morph_target);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    // --- DRIFT amount (smoothed) + weather step ---
    _drift = _drift_smooth.process(_drift_target);
    _step_weather();
    const float w = _weather * _drift;          // exactly 0 while drift is 0

    const float rate_drift_a = std::pow(2.f, kRateOct * kRateTap[0] * w);
    const float rate_drift_b = std::pow(2.f, kRateOct * kRateTap[1] * w);
    a.set_shape_offset(kShapeMax * kShapeTap[0] * w);
    b.set_shape_offset(kShapeMax * kShapeTap[1] * w);
    pa.set_detune_cents(kTuneCents * kTuneTap[0] * w);
    pb.set_detune_cents(kTuneCents * kTuneTap[1] * w);

    // --- COUPLE (Kuramoto PLL: convergence toward the geometric mean + phase pull) ---
    // Lock the RAW pitch phase — the sawtooth that clocks the loop/sequencer. NOT
    // phase_eff: that adds the per-bank EVOLVE offset _ev_phase, which a melody change
    // (MELO) zeroes at the phrase wrap. Locking phase_eff made every melody change
    // shove the raw clock to compensate — the "drifts when the melody changes" report.
    // The raw phase is continuous through a regen, so melody changes no longer disturb it.
    float dphi = a.pitch_phase() - b.pitch_phase();
    dphi -= std::floor(dphi + 0.5f);            // wrap to [-0.5, 0.5)
    _phase_err = dphi;

    const float fa = a.base_hz(), fb = b.base_hz();

    // convergence: both banks slide toward the geometric mean.
    const float conv_e = _couple * 0.5f;
    const float conv_a = std::pow(fb / fa, conv_e);
    const float conv_b = std::pow(fa / fb, conv_e);

    // phase pull: opposite sign on the two banks. At full COUPLE this becomes a
    // HARD lock — a much stronger gain (kKHard) with a per-tick slew cap
    // (kLockCap): the cap keeps engage click-free and the loop stable, while
    // the strong gain outruns EVOLVE's raw-rate wander so the residual phase
    // error collapses to ~0. Below full COUPLE the gentle kK nudge acts as
    // before (an approximate lock that lets the banks breathe).
    const bool hard = _couple >= kFullCouple;
    const float s = std::sin(TWO_PI * dphi);
    float corr = _couple * (hard ? kKHard : kK) * s;
    if (hard) corr = clampf(corr, -kLockCap, kLockCap);
    const float pull_a = 1.f - corr;
    const float pull_b = 1.f + corr;

    const float mult_a = clampf(conv_a * pull_a, kRateClampLo, kRateClampHi);
    const float mult_b = clampf(conv_b * pull_b, kRateClampLo, kRateClampHi);

    // single rate hook = COUPLE x DRIFT rate tap, applied symmetrically to
    // pitch and mod lanes (Tasks 4-5 split these apart)
    a.set_rate_scale(mult_a * rate_drift_a, mult_a * rate_drift_a);
    b.set_rate_scale(mult_b * rate_drift_b, mult_b * rate_drift_b);
}

void Center::_step_weather() {
    const float dt = static_cast<float>(kCtrlInterval) / _sr;   // control-tick period (s)
    if (_settle_ctr > 0) {
        --_settle_ctr;
        _ou *= _settle_coef;                    // panic: glide the walk to 0
    } else if (_drift > 0.f) {                   // no drift -> no weather system running
        // Ornstein-Uhlenbeck: mean-revert to 0, add scaled white noise. Noise
        // is scaled by _drift (not just gated by it > 0): the smoothed _drift
        // value asymptotically approaches but never exactly reaches 0, so a
        // bare boolean gate would let full-strength noise resume the instant
        // the SETTLE countdown (Task 7 checkpoint fix) expires, even while
        // _drift is still negligibly small — defeating SETTLE's purpose.
        _ou += (-_ou / kOuTau) * dt + kOuSigma * _drift * std::sqrt(dt) * _weather_rng.next_bipolar();
    }
    _weather = std::tanh(_ou);                  // softly bounded to (-1, 1)
}

void Center::spot(SuperModulator& a, SuperModulator& b) {
    a.spot(_spot_rng); b.spot(_spot_rng);
}

void Center::settle(SuperModulator& a, SuperModulator& b) {
    _drift_target = 0.f;
    _settle_ctr = static_cast<int>(_cr * 1.5f);
    a.settle(); b.settle();
}
