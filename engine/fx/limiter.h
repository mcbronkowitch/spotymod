#pragma once
#include <algorithm>
#include <cmath>

namespace spky {

// Master peak limiter (M4.6 dynamics spec). Gain-riding recipe after
// stmlib's Limiter (© Emilie Gillet, MIT — see THIRD_PARTY.md; no code
// copied verbatim), with three deliberate differences from the DaisySP
// copy: stereo-linked peak follower, EXACT bit-transparency below the
// knee at drive 0 (daisysp::Limiter applies SoftLimit(x*0.7)
// unconditionally), and a built-in master drive. Delivers the M6 shell
// spec's "Engine delta 3" (master soft-clip, transparent below ~-1 dBFS).
class Limiter {
public:
    void init() {
        _peak = 0.5f;
        _pre = 1.f;
    }
    void set_drive(float n) { _pre = 1.f + 3.f * std::clamp(n, 0.f, 1.f); }
    float pre_gain() const { return _pre; }

    void process(float& l, float& r) {
        const float pl = l * _pre, pr = r * _pre;
        const float peak = std::max(std::fabs(pl), std::fabs(pr));   // stereo link
        const float e = peak - _peak;
        _peak += (e > 0.f ? 0.05f : 0.00002f) * e;                   // stmlib slopes
        if (_peak < 1e-9f) _peak = 0.f;             // denormal floor (long silence)
        if (_pre == 1.f && _peak <= 1.f && peak <= kKnee)
            return;                                   // transparent: out == in, bit-exact
        const float gain = _peak > 1.f ? 1.f / _peak : 1.f;
        l = ceiling(pl * gain);
        r = ceiling(pr * gain);
    }

private:
    static constexpr float kKnee = 0.89125f;          // -1 dBFS

    // Piecewise ceiling: exact identity below the knee, tanh toward an
    // asymptote of exactly 1.0 above it (C1-continuous at the knee).
    static float ceiling(float x) {
        const float ax = std::fabs(x);
        if (ax <= kKnee) return x;
        const float y = kKnee + (1.f - kKnee) * std::tanh((ax - kKnee) / (1.f - kKnee));
        return x < 0.f ? -y : y;
    }

    float _peak = 0.5f;
    float _pre  = 1.f;
};

} // namespace spky
