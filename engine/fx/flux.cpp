#include "fx/flux.h"
#include "util/math.h"

using namespace spky;

namespace {
inline float dbfs2lin(float db) { return daisysp::pow10f(db * 0.05f); }
}

void Flux::init(float sample_rate, float* buf_l, float* buf_r) {
    _sw.init(sample_rate);
    _sr = sample_rate;
    _buf_ok = (buf_l != nullptr && buf_r != nullptr);
    if (!_buf_ok) return;
    _echo_l.Init(sample_rate, buf_l);
    _echo_r.Init(sample_rate, buf_r);
    _echo_l.SetLagTime(0.03f);   // short slew: click-free division changes, locks to grid
    _echo_r.SetLagTime(0.03f);
    _rate_idx = 3;               // boot "1/4"
    _bpm = 120.f;
    recompute_time(true);        // snap the boot time
    set_feedback(0.45f);
    set_mix(0.5f);
}

void Flux::set_bpm(float bpm) {
    if (bpm == _bpm) return;
    _bpm = bpm;
    recompute_time(false);
}

void Flux::set_rate(int slice_idx) {
    if (slice_idx == _rate_idx) return;
    _rate_idx = slice_idx;
    recompute_time(false);
}

void Flux::recompute_time(bool immediate) {
    if (!_buf_ok) return;
    int slice = _rate_idx < 0 ? 0
              : (_rate_idx >= kFluxRateCount ? kFluxRateCount - 1 : _rate_idx);
    float hz = division_hz(kFluxRateOffset + slice, _bpm);
    float t = (hz > 0.f) ? 1.f / hz : 0.5f;
    const float t_max = static_cast<float>(kMaxSamples - 2) / _sr;   // buffer safety
    _delay_time = clampf(t, 0.001f, t_max);
    _echo_l.SetDelayTime(_delay_time, immediate);
    _echo_r.SetDelayTime(_delay_time, immediate);
}

void Flux::set_feedback(float norm) {
    if (!_buf_ok) return;
    float fb = clampf(norm, 0.f, 1.f) * 1.1f;   // >1 allowed; SoftClip catches it
    _echo_l.SetFeedback(fb);
    _echo_r.SetFeedback(fb);
}

void Flux::set_mix(float norm) {
    if (!_buf_ok) return;
    _mix_lin = dbfs2lin(daisysp::fmap(clampf(norm, 0.f, 1.f), -40.f, 0.f));
}

void Flux::process(float& l, float& r) {
    if (!_buf_ok) return;
    float send = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry
    l += _echo_l.Process(l * send) * _mix_lin;
    r += _echo_r.Process(r * send) * _mix_lin;
}
