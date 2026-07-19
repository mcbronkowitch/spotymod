#include "fx/comp.h"
#include <algorithm>
#include <cmath>

using namespace spky;

namespace {
constexpr float kKneeDb     = 6.f;    // soft knee width
// THE by-ear loudness handle (spec: when in doubt, err toward loud).
constexpr float kMakeupComp = 0.9f;
// Post-comp envelope ceiling (-8 dBFS, ear-tunable): the crest headroom
// above the envelope is what keeps program peaks near 0 dBFS instead of
// grinding the master limiter (by-ear finding: the 0:26 clip in comp_pump).
constexpr float kEnvCeiling = 0.4f;

inline float coef_for(float time_s, float sr) {
    return 1.f - std::exp(-1.f / (time_s * sr));
}
} // namespace

void Comp::init(float sample_rate) {
    _sr = sample_rate;
    _amount.init(sample_rate, 0.002f);
    _amount.reset(0.f);
    _amount_target = 0.f;
    _att_coef  = coef_for(0.005f, sample_rate);
    _gain_coef = coef_for(0.002f, sample_rate);
    _gain_down_coef = coef_for(0.0005f, sample_rate);
    update_curve(0.f);
    _env = 0.f;
    _gain = _gain_target = 1.f;
    _ctr = 0;
}

void Comp::set_amount(float n) {
    _amount_target = std::clamp(n, 0.f, 1.f);
}

float Comp::gain_db() const { return 20.f * std::log10(_gain); }

void Comp::update_curve(float a) {
    _curve_amount = a;
    _thr_db = -32.f * a;
    const float ratio = 1.f + 9.f * a * a;          // 2:1 at 1/3, 5:1 at 2/3, 10:1 at 1
    _inv_ratio = 1.f / ratio;
    const float release_s = 0.06f + 0.29f * a * a;  // 60 ms at 0 .. 350 ms at 1 (~92 ms at glue)
    _rel_coef = coef_for(release_s, _sr);
    // Attack tightens with the knob (5 ms gentle .. 2 ms at full): at high
    // makeup the envelope must catch onsets before the ceiling cap can act,
    // or the transient leaks past full scale (the 0:26 onset residue).
    _att_coef = coef_for(0.005f - 0.003f * a, _sr);
    _makeup_db = -_thr_db * (1.f - _inv_ratio) * kMakeupComp;
}

void Comp::compute_gain() {
    if (_env < 1e-9f) _env = 0.f;   // denormal floor (long engaged silence)
    if (_amount.value() != _curve_amount) update_curve(_amount.value());
    const float env_db = 20.f * std::log10(std::max(_env, 1e-6f));
    const float over = env_db - _thr_db;
    float gr_db = 0.f;
    if (over >= kKneeDb * 0.5f) {
        gr_db = -over * (1.f - _inv_ratio);
    } else if (over > -kKneeDb * 0.5f) {            // quadratic soft knee
        const float t = over + kKneeDb * 0.5f;
        gr_db = -(t * t) / (2.f * kKneeDb) * (1.f - _inv_ratio);
    }
    _gain_target = std::pow(10.f, (gr_db + _makeup_db) / 20.f);
    // Never command a gain that lifts the envelope above the post-comp
    // ceiling; quiet material sits far below the cap and keeps its full
    // makeup, so the loudness intent survives.
    const float cap = kEnvCeiling / std::max(_env, 1e-6f);
    if (_gain_target > cap) _gain_target = cap;
}

void Comp::process(float& l, float& r) {
    if (!engaged()) {
        if (_gain != 1.f) {                          // re-arm after disengage
            _gain = _gain_target = 1.f;
            _env = 0.f;
            _ctr = 0;
        }
        return;                                      // bit-exact bypass
    }
    _amount.process(_amount_target);
    const float peak = std::max(std::fabs(l), std::fabs(r));   // stereo link
    _env += (peak > _env ? _att_coef : _rel_coef) * (peak - _env);
    if (_ctr == 0) { compute_gain(); _ctr = kDecimate; }
    --_ctr;
    // Asymmetric smoothing: downward moves (into the cap / into reduction)
    // act in ~0.5 ms so swells cannot outrun the ceiling; upward recovery
    // keeps the gentler 2 ms.
    _gain += (_gain_target < _gain ? _gain_down_coef : _gain_coef)
             * (_gain_target - _gain);
    l *= _gain;
    r *= _gain;
}
