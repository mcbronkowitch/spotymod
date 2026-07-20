#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "Utility/dsp.h"
#include "fx/taps.h"
#include "fx/fx_util.h"
#include "mod/divisions.h"
#include "util/fast_tanh.h"

namespace spky {

// Port of src/core/deline.h (interpolating delay line over an injected buffer).
// max_size must be a power of two: all index wraps are AND masks (the indices
// are never negative), so the per-sample path stays free of integer division.
template <typename T, size_t max_size>
class DeLine {
    static_assert((max_size & (max_size - 1)) == 0, "max_size must be a power of two");
    static constexpr int32_t kMask = static_cast<int32_t>(max_size) - 1;

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
        delay_ = int_delay & kMask;
    }

    void Write(T sample) {
        line_[write_ptr_] = sample;
        write_ptr_ = (write_ptr_ - 1) & kMask;
    }

    T Read() const {
        T a = line_[(write_ptr_ + delay_) & kMask];
        T b = line_[(write_ptr_ + delay_ + 1) & kMask];
        return a + (b - a) * frac_;
    }

    // Raw access for TapBank's read taps (TapBank reads integer offsets
    // behind the write head; no interpolation, no state).
    const T* data() const { return line_; }
    int32_t write_ptr() const { return write_ptr_; }

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
// Feedback unbounded but soft-clipped, output full-wet, band-passed. The
// delay-time slew (the "tape" pitch behavior — under modulation this slew IS
// the feature: FLOW = wobble, STEP = dub pitch jumps) lives in Flux, which
// runs one shared one-pole for both channels and hands the smoothed length
// in as delay_samples.
template <size_t max_size>
class EchoDelay {
public:
    EchoDelay() = default;
    EchoDelay(const EchoDelay&) = delete;
    EchoDelay& operator=(const EchoDelay&) = delete;

    void Init(float sample_rate, float* buf) {
        delay_line_.Init(buf);
        bpf_.Init(sample_rate);
        feedback_ = 0.f;
    }

    void SetFeedback(float feedback) { feedback_ = feedback; }
    float Feedback() const { return feedback_; }

    const float* line() const { return delay_line_.data(); }
    int32_t write_ptr() const { return delay_line_.write_ptr(); }

    // Mind the parameter ORDER: `delay_samples` is the second argument and has
    // been since 8723bc5 lifted the delay-time slew into Flux -- one shared
    // one-pole for both channels instead of two. EchoDelay owns no sample rate
    // and no smoother; the caller passes an already-slewed length in samples.
    float Process(float in, float delay_samples) {
        delay_line_.SetDelay(delay_samples);
        float out = delay_line_.Read();
        out = bpf_.Process(out);
        out = fast_tanh(out);   // tape-warm limiter: transparent near unity,
                                // bounded self-oscillation above it (bloom).
                                // The bound is now a hard clamp at |x| >= 3.65
                                // rather than tanh's asymptote -- feedback runs
                                // to 1.2, so |y| <= 1 is what keeps this loop
                                // stable (util/fast_tanh.h).
        delay_line_.Write(out * feedback_ + in);
        return out;
    }

private:
    float feedback_ = 0.f;

    DeLine<float, max_size> delay_line_;
    TapeBpf bpf_;
};

// FLUX block: the stereo tape echo behind a click-free SoftSwitch, echo added
// onto the signal at FLUX MIX (original topology: send-style, full-wet echo).
class Flux {
public:
    // Power of two so DeLine's index wraps compile to AND masks instead of
    // integer divisions (4 modulos per sample per channel otherwise).
    // 2^18 = 5.46 s @ 48 kHz; was 240000 (5 s) — the extra 0.35 MB SDRAM per
    // buffer is the price of the mask.
    static constexpr size_t kMaxSamples = 262144;

    void init(float sample_rate, float* buf_l, float* buf_r);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    bool engaged() const {
        return _buf_ok && (_sw.is_on() || !_sw.is_idle());
    }
    bool has_buffers() const { return _buf_ok; }
    void set_bpm(float bpm);
    void set_rate(int slice_idx);
    float delay_time() const { return _delay_time; }
    void set_feedback(float norm);
    void set_mix(float norm);
    void set_dust(float norm);                           // 0..1 tap morph
    void set_rot(float norm);                            // 0..1 spectral spread
    // Tap offsets in samples behind the write head, from the OTHER bank's
    // rhythm. Pushed at control rate by Instrument; see fx/taps.h.
    void set_tap_offsets(const int32_t off[tap_tuning::kTaps]);
    bool taps_active() const { return _taps.active(); }
    void process(float& l, float& r);

private:
    void recompute_time(bool immediate);

    EchoDelay<kMaxSamples> _echo_l;
    EchoDelay<kMaxSamples> _echo_r;
    TapBank _taps;
    SoftSwitch _sw;
    float _mix_lin = 0.f;
    bool _buf_ok = false;
    float _sr = 48000.f;
    float _bpm = 120.f;
    int   _rate_idx = 3;         // "1/4"
    float _delay_time = 0.5f;
    // shared L/R delay-time slew (both channels always run the same length)
    float _dt_current = 0.05f;   // seconds
    float _dt_target = 0.05f;
    float _dt_coef = 1.f;
    // Unchanged-value guards for set_dust/set_rot (I3), mirroring set_bpm/
    // set_rate: TapBank::set_rot runs two powf calls, and once these are
    // forwarded at control rate every tick would pay that whether or not the
    // knob moved. Defaults match TapBank::init's own _dust=0/_rot=0 so the
    // first real call after a FRESH Flux only skips forwarding when it is a
    // no-op -- but init() can also run again later (a sample-rate change),
    // and TapBank::init resets _dust/_rot back to 0 every time. Flux::init
    // resets these two guards to 0 right alongside them (flux.cpp), same as
    // it does for _bpm/_rate_idx, so a re-init never leaves a guard stale
    // against a TapBank that just went back to its own defaults.
    float _dust_norm = 0.f;
    float _rot_norm = 0.f;
};

} // namespace spky
