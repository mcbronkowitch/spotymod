#pragma once
#include "util/onepole.h"

namespace spky {

// One-knob compressor (M4.6 dynamics spec). Turning up = deeper threshold,
// higher ratio, slower release, auto-makeup: glue at ~1/3, dense at ~2/3,
// audible pumping in the top third. The knob is a loudness knob first —
// quiet material must come UP as the knob comes up.
//
// Cost control: detector + gain smoothing run per sample; the dB-domain
// gain computer (the only log/pow code) runs decimated every kDecimate
// samples (spec: ~0.3 % for both parts).
class Comp {
public:
    void init(float sample_rate);
    void set_amount(float n);                 // 0..1; 0 = bit-exact bypass
    float amount() const { return _amount_target; }
    // true while audibly processing: target > 0 or still smoothing to 0
    bool engaged() const { return _amount_target > 0.f || _amount.value() > kEps; }
    float gain_db() const;                    // applied gain incl. makeup (tests/meter)
    void process(float& l, float& r);

private:
    static constexpr int   kDecimate = 16;
    static constexpr float kEps      = 1e-4f;

    void update_curve(float a);               // amount -> thr/ratio/release/makeup
    void compute_gain();                      // decimated dB-domain gain computer

    OnePole _amount;                          // ~2 ms knob smoothing
    float _sr            = 48000.f;
    float _amount_target = 0.f;
    // curve (recomputed only when the smoothed amount actually moved — the
    // OnePole snaps on convergence, so a static knob costs no exp/pow here)
    float _curve_amount = -1.f;               // amount the curve was built for
    float _thr_db    = 0.f;
    float _inv_ratio = 1.f;                   // 1/ratio
    float _makeup_db = 0.f;
    // detector (stereo-linked linear peak envelope)
    float _env      = 0.f;
    float _att_coef = 0.f;                    // 5 ms at amount 0 .. 2 ms at 1
    float _rel_coef = 0.f;                    // from the knob
    // gain
    float _gain        = 1.f;                 // per-sample smoothed linear gain
    float _gain_target = 1.f;
    float _gain_coef   = 0.f;                 // ~2 ms (upward recovery)
    float _gain_down_coef = 0.f;              // ~0.5 ms (downward, enforces the ceiling)
    int   _ctr         = 0;                   // decimation counter
};

} // namespace spky
