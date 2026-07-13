#include "fx/part_fx.h"
#include <cmath>

using namespace spky;

void PartFx::init(float sample_rate, float* echo_l, float* echo_r) {
    _grit.init(sample_rate);
    _flux.init(sample_rate, echo_l, echo_r);
    _comp.init(sample_rate);
    for (auto& s : _smooth) s.init(sample_rate, 0.002f);
    _grit_applied = -1.f;
    _primed = false;
}

void PartFx::set_fx_on(FxBlock b, bool on, bool immediate) {
    if (b == FxBlock::Flux) _flux.set_on(on, immediate);
    else                    _grit.set_on(on, immediate);
}

void PartFx::process(float& l, float& r, float& send_l, float& send_r,
                     const float* fxv) {
    if (!_primed) {   // snap to the first real values: no phantom boot slew
        for (int i = 0; i < FXT_COUNT; ++i) _smooth[i].reset(fxv[i]);
        _primed = true;
    }
    float v[FXT_COUNT];
    for (int i = 0; i < FXT_COUNT; ++i) v[i] = _smooth[i].process(fxv[i]);

    if (_grit.engaged() || _flux.engaged()) {
        if (v[FXT_GRIT_INT] != _grit_applied) {
            _grit.set_intensity(v[FXT_GRIT_INT]);
            _grit_applied = v[FXT_GRIT_INT];
        }
        _flux.set_time(v[FXT_FLUX_TIME]);      // slewed inside EchoDelay (tape)
        _flux.set_feedback(v[FXT_FLUX_FB]);
        const float dry_l = l, dry_r = r;
        _grit.process(l, r);
        _flux.process(l, r);
        const float m = v[FXT_FX_MIX];
        l = dry_l + (l - dry_l) * m;
        r = dry_r + (r - dry_r) * m;
    }

    _comp.process(l, r);   // one-knob comp — BEFORE the send tap (spec: full-wet must profit)

    const float g = std::sin(v[FXT_REV_SEND] * 1.5707963f);   // equal-power
    send_l = l * g;
    send_r = r * g;
}
