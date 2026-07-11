#include "fx/reverb.h"
#include "Utility/dsp.h"
#include "util/math.h"

using namespace spky;

void AmbientReverb::init(float sample_rate) {
    _rev.Init(sample_rate);
    _shift.Init(sample_rate);
    _shift.SetTransposition(12.f);   // fixed +1 octave (spec)
    _shift.SetFun(0.f);
    _shim = 0.f;
    _last_l = _last_r = 0.f;
    set_size(0.7f);                  // boot defaults (spec)
    set_tone(0.5f);
}

void AmbientReverb::set_size(float norm) {
    _rev.SetFeedback(daisysp::fmap(clampf(norm, 0.f, 1.f), 0.4f, 0.99f));
}

void AmbientReverb::set_tone(float norm) {
    _rev.SetLpFreq(daisysp::fmap(clampf(norm, 0.f, 1.f), 500.f, 16000.f,
                                 daisysp::Mapping::LOG));
}

void AmbientReverb::set_shimmer(float norm) {
    _shim = clampf(norm, 0.f, 1.f);
}

void AmbientReverb::process(float in_l, float in_r, float& out_l, float& out_r) {
    if (_shim > 0.f) {
        float mono = 0.5f * (_last_l + _last_r);
        float shifted = _shift.Process(mono);
        float g = _shim * 0.5f;      // fixed headroom on the feedback path
        in_l += shifted * g;
        in_r += shifted * g;
    }
    _rev.Process(in_l, in_r, &out_l, &out_r);
    _last_l = out_l;
    _last_r = out_r;
}
