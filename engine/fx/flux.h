#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "Utility/dsp.h"
#include "fx/dust.h"
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

    // Raw access for the grain taps (DustCloud reads integer offsets behind
    // the write head; no interpolation, no state).
    const T* data() const { return line_; }
    int32_t write_ptr() const { return write_ptr_; }

    // Move the head without storing — FREEZE: the tape keeps rolling under
    // the read heads, but nothing is written onto it.
    void Advance() {
        write_ptr_ = (write_ptr_ - 1) & kMask;
    }

    // EROSION: abrade what is on the tape and burn `sample` into it. `wear`
    // slightly below 1 is what makes the frozen loop decompose per pass.
    void WriteBlend(T sample, float wear) {
        // fast_tanh, not std::tanh: this runs per sample in the audio path
        // while frozen, and flux.h already includes util/fast_tanh.h.
        line_[write_ptr_] = fast_tanh(line_[write_ptr_] * wear + sample);
        write_ptr_ = (write_ptr_ - 1) & kMask;
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
        frozen_ = false;
        wear_ = 1.f;
    }

    void SetFeedback(float feedback) { feedback_ = feedback; }
    float Feedback() const { return feedback_; }

    void set_freeze(bool on) { frozen_ = on; }
    bool frozen() const { return frozen_; }
    // Per-sample erosion coefficient applied to the tape while frozen.
    // 1.0 = preserving looper; slightly below 1 = the loop decomposes.
    void set_wear(float w) { wear_ = w; }

    const float* line() const { return delay_line_.data(); }
    int32_t write_ptr() const { return delay_line_.write_ptr(); }

    // `wb` is the DustCloud writeback (already bounded by the caller); it is 0
    // outside ROT zone R, and the store is then bit-identical to the pre-DUST
    // path. DEFAULTED, so every existing call site compiles unchanged.
    //
    // Mind the parameter ORDER: `delay_samples` is the second argument and has
    // been since 8723bc5 lifted the delay-time slew into Flux -- one shared
    // one-pole for both channels instead of two. EchoDelay owns no sample rate
    // and no smoother; the caller passes an already-slewed length in samples.
    float Process(float in, float delay_samples, float wb = 0.f) {
        delay_line_.SetDelay(delay_samples);
        float out = delay_line_.Read();
        out = bpf_.Process(out);
        out = fast_tanh(out);   // tape-warm limiter: transparent near unity,
                                // bounded self-oscillation above it (bloom).
                                // The bound is now a hard clamp at |x| >= 3.65
                                // rather than tanh's asymptote -- feedback runs
                                // to 1.2, so |y| <= 1 is what keeps this loop
                                // stable (util/fast_tanh.h).
        if (!frozen_) {
            float store = out * feedback_ + in;
            if (wb != 0.f) store += wb;   // explicit: keeps the wb == 0 path
            delay_line_.Write(store);     // bit-identical, not merely equal
        } else if (wear_ >= 1.f && wb == 0.f) {
            delay_line_.Advance();        // preserving looper
        } else {
            delay_line_.WriteBlend(wb, wear_);   // erosion
        }
        return out;
    }

private:
    float feedback_ = 0.f;
    bool  frozen_ = false;
    float wear_ = 1.f;

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

    // `seed` is a caller-supplied constant, not derived from `buf_l`/`buf_r`'s
    // address (see the comment on the call to _dust.init() in flux.cpp for
    // why: the address is not reproducible in the VCV plugin). Part::init
    // (via PartFx::init) hands each part a distinct constant, the same
    // idiom Instrument::init already uses for SynthEngine's drift seed.
    void init(float sample_rate, float* buf_l, float* buf_r, uint32_t seed);
    void set_on(bool on, bool immediate = false) { _sw.set_on(on, immediate); }
    bool is_on() const { return _sw.is_on(); }
    bool engaged() const {
        return _buf_ok && (_sw.is_on() || !_sw.is_idle());
    }
    bool has_buffers() const { return _buf_ok; }
    void set_bpm(float bpm);              // recompute synced delay time on change
    void set_rate(int slice_idx);         // 0..kFluxRateCount-1 -> kDivisions slice
    float delay_time() const { return _delay_time; }   // seconds, clamped (test/meter)
    void set_feedback(float norm);                       // 0 .. 1.2 (tanh-bounded bloom)
    void set_mix(float norm);                            // -40 .. 0 dBFS
    void set_dust(float norm);                           // 0 .. 1 grain amount
    void set_rot(float norm);                            // 0 .. 1 character
    bool dust_active() const { return _dust.active(); }
    void process(float& l, float& r);

private:
    void recompute_time(bool immediate);

    EchoDelay<kMaxSamples> _echo_l;
    EchoDelay<kMaxSamples> _echo_r;
    DustCloud _dust;
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
    // set_rate: DustCloud::_remap() runs a pow + a cos, and once these are
    // forwarded at control rate every tick would pay that whether or not the
    // knob moved. Defaults match DustCloud::init's own _dust=0/_rot=0, so the
    // first real call after init() only skips forwarding when it is a no-op.
    float _dust_norm = 0.f;
    float _rot_norm = 0.f;
};

} // namespace spky
