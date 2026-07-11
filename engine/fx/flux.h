#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "Utility/dsp.h"
#include "fx/fx_util.h"

namespace spky {

// Port of src/core/deline.h (interpolating delay line over an injected buffer).
template <typename T, size_t max_size>
class DeLine {
public:
    DeLine() = default;
    DeLine(const DeLine&) = delete;
    DeLine& operator=(const DeLine&) = delete;

    void Init(float* buf) {
        line_ = buf;
        Reset();
    }

    void Reset() {
        std::memset(line_, 0, max_size * sizeof(T));
        write_ptr_ = 0;
        delay_ = 1;
        frac_ = 0.f;
    }

    void SetDelay(float delay) {
        int32_t int_delay = static_cast<int32_t>(delay);
        frac_ = delay - static_cast<float>(int_delay);
        delay_ = int_delay % static_cast<int32_t>(max_size);
    }

    void Write(T sample) {
        line_[write_ptr_] = sample;
        write_ptr_ = (write_ptr_ - 1 + max_size) % max_size;
    }

    T Read() const {
        T a = line_[(write_ptr_ + delay_) % max_size];
        T b = line_[(write_ptr_ + delay_ + 1) % max_size];
        return a + (b - a) * frac_;
    }

private:
    float frac_ = 0.f;
    int32_t write_ptr_ = 0;
    int32_t delay_ = 1;
    T* line_ = nullptr;
};

// The echo's tape band-limit: the one biquad the original chain uses —
// a single band-pass section at 800 Hz with the effective Q of 0.1 that
// src/core/echo.h's SetParams(800, 0) produced. Transposed direct form 2;
// coefficient math from src/core/biquad.cpp.
class TapeBpf {
public:
    void Init(float sample_rate) {
        constexpr float pi = 3.14159265358979f;
        constexpr float cutoff_hz = 800.f;
        constexpr float q = 0.1f;
        const float k = std::tan(pi * cutoff_hz / sample_rate);
        const float ksq = k * k;
        const float norm = 1.f / (1.f + (k / q) + ksq);
        b0_ = (k / q) * norm;
        a1_ = 2.f * (ksq - 1.f) * norm;
        a2_ = (1.f - (k / q) + ksq) * norm;
        s1_ = s2_ = 0.f;
    }

    float Process(float in) {
        // b1 = 0, b2 = -b0 for the band-pass case
        float y = b0_ * in + s1_;
        s1_ = s2_ - a1_ * y;
        s2_ = -b0_ * in - a2_ * y;
        return y;
    }

private:
    float b0_ = 0.f, a1_ = 0.f, a2_ = 0.f;
    float s1_ = 0.f, s2_ = 0.f;
};

// Port of src/core/echo.h (Nick Donaldson / Infrasonic Audio): tape-ish echo.
// Feedback unbounded but soft-clipped, output full-wet, band-passed, delay
// time changes slewed by a one-pole (the "tape" pitch behavior — under
// modulation this slew IS the feature: FLOW = wobble, STEP = dub pitch jumps).
template <size_t max_size>
class EchoDelay {
public:
    EchoDelay() = default;
    EchoDelay(const EchoDelay&) = delete;
    EchoDelay& operator=(const EchoDelay&) = delete;

    void Init(float sample_rate, float* buf) {
        sample_rate_ = sample_rate;
        delay_line_.Init(buf);
        bpf_.Init(sample_rate);
        delay_time_current_ = delay_time_target_ = 0.05f;
        feedback_ = 0.f;
    }

    // Approximate lag (smoothing) for delay-time changes, in seconds.
    void SetLagTime(float time_s) {
        delay_smooth_coef_ = (time_s <= 0.f || sample_rate_ <= 0.f)
            ? 1.f
            : daisysp::fmin(1.f / (time_s * sample_rate_), 1.f);
    }

    void SetDelayTime(float time_s, bool immediately = false) {
        delay_time_target_ = time_s;
        if (immediately) delay_time_current_ = time_s;
    }

    void SetFeedback(float feedback) { feedback_ = feedback; }
    float Feedback() const { return feedback_; }

    float Process(float in) {
        daisysp::fonepole(delay_time_current_, delay_time_target_,
                          delay_smooth_coef_);
        delay_line_.SetDelay(delay_time_current_ * sample_rate_);
        float out = delay_line_.Read();
        out = bpf_.Process(out);
        out = daisysp::SoftClip(out);
        delay_line_.Write(out * feedback_ + in);
        return out;
    }

private:
    float sample_rate_ = 48000.f;
    float delay_time_current_ = 0.05f;
    float delay_time_target_ = 0.05f;
    float delay_smooth_coef_ = 1.f;
    float feedback_ = 0.f;

    DeLine<float, max_size> delay_line_;
    TapeBpf bpf_;
};

// FLUX block: the stereo tape echo behind a click-free SoftSwitch, echo added
// onto the signal at FLUX MIX (original topology: send-style, full-wet echo).
class Flux {
public:
    static constexpr size_t kMaxSamples = 240000;   // 5 s @ 48 kHz

    void init(float sample_rate, float* buf_l, float* buf_r);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    bool engaged() const {
        return _buf_ok && (_sw.is_on() || !_sw.is_idle());
    }
    bool has_buffers() const { return _buf_ok; }
    void set_time(float norm, bool immediate = false);   // 50 ms .. 5 s, exp
    void set_feedback(float norm);                       // 0 .. 1.1
    void set_mix(float norm);                            // -40 .. 0 dBFS
    void process(float& l, float& r);

private:
    EchoDelay<kMaxSamples> _echo_l;
    EchoDelay<kMaxSamples> _echo_r;
    SoftSwitch _sw;
    float _mix_lin = 0.f;
    bool _buf_ok = false;
};

} // namespace spky
