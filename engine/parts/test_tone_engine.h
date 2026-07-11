#pragma once
#include <cmath>
#include "parts/engine_iface.h"
#include "util/math.h"

namespace spky {

// Minimal audible engine for M1: a sine whose pitch follows the PITCH target
// and whose amplitude follows the LEVEL target. Replaced by the real engines in
// later milestones.
//
// Pitch contract: normalized 0..1 == 36 semitones (3 octaves), 110..880 Hz.
// The PITCH target is already quantized-and-tuned upstream in Part (tune is
// summed before quantization), so it maps straight to frequency here with no
// scaling - anything else would warp the scale grid off equal temperament.
class TestToneEngine : public IPartEngine {
public:
    void init(float sample_rate) override { _sr = sample_rate; _phase = 0.f; }

    void set_targets(const float* t, float /*tune*/) override {
        float p = clampf(t[LANE_PITCH], 0.f, 1.f);
        _freq = 110.f * std::pow(8.f, p);   // 110..880 Hz, exact equal temperament
        _amp  = t[LANE_LEVEL];
    }

    void trigger(float /*pitch_norm*/) override {}   // test tone is continuous

    void process(float& outL, float& outR) override {
        _phase += _freq / _sr;
        if (_phase >= 1.f) _phase -= 1.f;
        float s = std::sin(_phase * TWO_PI) * _amp * 0.3f;
        outL = s;
        outR = s;
    }

private:
    float _sr = 48000.f;
    float _phase = 0.f;
    float _freq = 220.f;
    float _amp = 0.5f;
};

} // namespace spky
