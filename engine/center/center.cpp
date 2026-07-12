#include "center/center.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kQuarter = TWO_PI * 0.25f;   // pi/2, for the equal-power law
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

    // COUPLE and DRIFT arrive in Tasks 5-6; keep the hooks at their zero-effect
    // values so pre-M4 behavior is bit-identical.
    a.set_rate_scale(1.f);   b.set_rate_scale(1.f);
    a.set_shape_offset(0.f); b.set_shape_offset(0.f);
    pa.set_detune_cents(0.f); pb.set_detune_cents(0.f);
    (void)pa; (void)pb;
}

void Center::_step_weather() { /* filled in Task 5 */ }

void Center::spot(SuperModulator& a, SuperModulator& b) {
    a.spot(_spot_rng); b.spot(_spot_rng);
}

void Center::settle(SuperModulator& a, SuperModulator& b) {
    _drift_target = 0.f;
    _settle_ctr = static_cast<int>(_cr * 1.5f);
    a.settle(); b.settle();
}
