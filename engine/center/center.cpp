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
}

void Center::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _cr = sample_rate / static_cast<float>(kCtrlInterval);

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

    // --- COUPLE multiplier (unity until Task 6) ---
    const float mult_a = 1.f;
    const float mult_b = 1.f;

    // single rate hook = COUPLE x DRIFT rate tap
    a.set_rate_scale(mult_a * rate_drift_a);
    b.set_rate_scale(mult_b * rate_drift_b);
}

void Center::_step_weather() {
    const float dt = static_cast<float>(kCtrlInterval) / _sr;   // control-tick period (s)
    if (_settle_ctr > 0) {
        --_settle_ctr;
        _ou *= _settle_coef;                    // panic: glide the walk to 0
    } else if (_drift > 0.f) {                   // no drift -> no weather system running
        // Ornstein-Uhlenbeck: mean-revert to 0, add scaled white noise.
        _ou += (-_ou / kOuTau) * dt + kOuSigma * std::sqrt(dt) * _weather_rng.next_bipolar();
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
