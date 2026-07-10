#pragma once
#include <cmath>

namespace spky {

// Portable one-pole smoother (same math as the firmware's OnePoleSmoother,
// but with no libDaisy dependency).
class OnePole {
public:
    void init(float sample_rate, float time_s = 0.001f) {
        if (time_s <= 0.f || sample_rate <= 0.f) { _kof = 1.f; return; }
        float k = 1.f / (time_s * sample_rate);
        _kof = k > 1.f ? 1.f : k;
    }

    float process(float target) {
        float diff = target - _value;
        if (!_smoothing && std::fabs(diff) < 0.0005f) return _value;
        _smoothing = true;
        _value += _kof * diff;
        if (std::fabs(target - _value) < 0.0005f) {
            _value = target;
            _smoothing = false;
        }
        return _value;
    }

    void reset(float value = 0.f) { _value = value; _smoothing = false; }
    float value() const { return _value; }

private:
    float _kof = 1.f;
    float _value = 0.f;
    bool  _smoothing = false;
};

} // namespace spky
