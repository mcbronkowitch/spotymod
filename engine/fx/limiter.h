#pragma once
#include <algorithm>
#include <cmath>
#include "util/fast_tanh.h"

namespace spky {

// Master peak limiter + DRIVE saturator (M4.6 dynamics spec). Gain-riding
// recipe after stmlib's Limiter (© Emilie Gillet, MIT — see THIRD_PARTY.md;
// no code copied verbatim), with deliberate differences from the DaisySP
// copy: stereo-linked peak follower, EXACT bit-transparency below the knee
// at drive 0 (daisysp::Limiter applies SoftLimit(x*0.7) unconditionally),
// and a built-in master DRIVE. Delivers the M6 shell spec's "Engine delta 3"
// (master soft-clip, transparent below ~-1 dBFS).
//
// DRIVE is a sound-design tool, not just make-up gain: the ceiling knee
// MORPHS with drive. At drive 0 it is the transparent -1 dBFS hard knee
// (bit-exact below it); as DRIVE rises the knee slides down and its tanh
// transition widens, so the master bus turns into a warm, wide-knee
// saturator. A hard-driven mix then gains warm harmonics smoothly instead
// of hard-clipping transient overshoots into audible crackle (by-ear: high
// RES + COMP pumped resonant peaks past the old fixed knee and it crackled).
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
        // DRIVE morphs the knee: 0.89 (transparent) at drive 0 -> 0.45 (warm)
        // at full drive. Lower knee => the tanh bends earlier and its (1-knee)
        // transition is wider => softer, warmer saturation of the driven peaks.
        const float drive = (_pre - 1.f) * (1.f / 3.f);              // 0..1
        const float knee  = kKneeHi - (kKneeHi - kKneeLo) * drive;
        if (_pre == 1.f && _peak <= 1.f && peak <= knee)
            return;                                   // transparent: out == in, bit-exact
        const float gain = _peak > 1.f ? 1.f / _peak : 1.f;
        l = shape(pl * gain, knee);
        r = shape(pr * gain, knee);
    }

private:
    static constexpr float kKneeHi = 0.89125f;        // -1 dBFS: transparent at drive 0
    static constexpr float kKneeLo = 0.45f;           // warm saturation onset at full drive

    // Wide-knee soft saturation: exact identity below the knee, tanh toward an
    // asymptote of 1.0 above it (C1-continuous at the knee). With knee = kKneeHi
    // this is exactly the original -1 dBFS ceiling; a lower knee starts the bend
    // earlier over a wider (1-knee) transition => the warm DRIVE curve.
    static float shape(float x, float knee) {
        const float ax = std::fabs(x);
        if (ax <= knee) return x;
        const float y = knee + (1.f - knee) * fast_tanh((ax - knee) / (1.f - knee));
        return x < 0.f ? -y : y;
    }

    float _peak = 0.5f;
    float _pre  = 1.f;
};

} // namespace spky
