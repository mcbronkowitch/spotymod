#pragma once
#include <cmath>
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// Band-limited morphing oscillator: ONE phasor, continuous morph
// sine -> triangle -> saw -> pulse (anchors at morph 0, 1/3, 2/3, 1), with
// polyblep corrections on the saw and pulse discontinuities. Shapes are
// phase-aligned (sine and triangle peak at phase 0.25; saw crosses zero
// rising at 0.5; pulse is +1 on the first half) so the morph never jumps.
// The triangle is naive (derivative discontinuity only) - v1 scope; the
// spec flags "polyblep the tri->saw midpoint" as an assumption to verify
// by ear, not a requirement.
//
// Audio path uses fast_sin only (no libm sinf - CPU budget constraint).
// set_detune_cents uses std::pow: control-rate only, never per sample.
class MorphOsc {
public:
    void init(float sample_rate) {
        _sr = sample_rate;
        _phase = 0.f;
        _ratio = 1.f;
        set_freq(220.f);
    }

    void set_freq(float hz) {
        _freq = hz < 0.f ? 0.f : hz;
        _inc = _freq * _ratio / _sr;
    }

    void set_detune_cents(float ct) {                    // control rate
        // Voice::_apply_freq pushes this twice per voice per control tick, i.e.
        // 32 libm powf per 96-sample block on the two-part instrument. The
        // argument only moves when TIMBRE moves or when the per-voice drift LFO
        // is running (DRIFT/MOTION width > 0), so on a still patch every one of
        // those recomputed the ratio it already had. Exact guard, no approximation.
        if (ct == _ct) return;
        _ct = ct;
        _ratio = std::pow(2.f, ct * (1.f / 1200.f));
        _inc = _freq * _ratio / _sr;
    }

    void set_morph(float m) { _morph = clampf(m, 0.f, 1.f); }

    void reset_phase(float ph = 0.f) { _phase = clampf(ph, 0.f, 0.999999f); }

    float process() {
        _phase += _inc;
        if (_phase >= 1.f) _phase -= 1.f;
        const float t = _phase;
        const float dt = _inc > 1e-6f ? _inc : 1e-6f;
        const float seg = _morph * 3.f;
        if (seg <= 1.f) return lerpf(fast_sin(t), _tri(t), seg);
        if (seg <= 2.f) return lerpf(_tri(t), _saw(t, dt), seg - 1.f);
        return lerpf(_saw(t, dt), _pulse(t, dt), seg - 2.f);
    }

private:
    // triangle phase-aligned with the sine: peak +1 at t=0.25, -1 at t=0.75
    static float _tri(float t) {
        if (t < 0.25f) return 4.f * t;
        if (t < 0.75f) return 2.f - 4.f * t;
        return 4.f * t - 4.f;
    }

    // standard 2-sample polyblep residual around a phase-wrap discontinuity
    static float _polyblep(float t, float dt) {
        if (t < dt) {
            float x = t / dt;
            return x + x - x * x - 1.f;
        }
        if (t > 1.f - dt) {
            float x = (t - 1.f) / dt;
            return x * x + x + x + 1.f;
        }
        return 0.f;
    }

    // saw rising through zero at t=0.5; -2 step at the wrap, blep-corrected
    static float _saw(float t, float dt) { return 2.f * t - 1.f - _polyblep(t, dt); }

    // 50% pulse: +2 step at the wrap, -2 step at t=0.5, both blep-corrected
    static float _pulse(float t, float dt) {
        float t2 = t + 0.5f;
        if (t2 >= 1.f) t2 -= 1.f;
        float v = t < 0.5f ? 1.f : -1.f;
        return v + _polyblep(t, dt) - _polyblep(t2, dt);
    }

    float _sr = 48000.f;
    float _phase = 0.f;
    float _freq = 220.f;
    float _ratio = 1.f;
    float _ct = 0.f;             // last set_detune_cents argument (change guard)
    float _inc = 0.f;
    float _morph = 0.f;
};

} // namespace spky
