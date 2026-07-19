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
    // short slew: click-free division changes, locks to grid (~30 ms lag)
    _dt_coef = daisysp::fmin(1.f / (0.03f * sample_rate), 1.f);
    _rate_idx = 3;               // boot "1/4"
    _bpm = 120.f;
    recompute_time(true);        // snap the boot time
    set_feedback(0.45f);
    set_mix(0.5f);
    // Fixed distinct per-part seeds: the buffer pointer is stable per part for
    // the life of the instrument, so hashing it gives A and B different grain
    // streams while staying bit-reproducible across desktop and firmware.
    _dust.init(sample_rate, 0xD0571u ^ (uint32_t)(uintptr_t)buf_l);
    _dust.set_delay_time(_delay_time);
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
    _dt_target = _delay_time;
    if (immediate) _dt_current = _delay_time;
    _dust.set_delay_time(_delay_time);   // zone S grid follows the echo
}

void Flux::set_feedback(float norm) {
    if (!_buf_ok) return;
    float fb = clampf(norm, 0.f, 1.f) * 1.2f;   // up to ~120%; tanh loop bounds the bloom
    _echo_l.SetFeedback(fb);
    _echo_r.SetFeedback(fb);
}

void Flux::set_mix(float norm) {
    if (!_buf_ok) return;
    _mix_lin = dbfs2lin(daisysp::fmap(clampf(norm, 0.f, 1.f), -40.f, 0.f));
}

void Flux::set_dust(float norm) {
    if (!_buf_ok) return;
    _dust.set_dust(clampf(norm, 0.f, 1.f));
}

void Flux::set_rot(float norm) {
    if (!_buf_ok) return;
    _dust.set_rot(clampf(norm, 0.f, 1.f));
}

void Flux::process(float& l, float& r) {
    if (!_buf_ok) return;
    float send = _sw.process();
    if (_sw.is_idle()) return;   // fully off: bit-exact dry

    // The shared delay-time slew (8723bc5) advances exactly ONCE per sample,
    // before the branch -- both paths must see the same tape geometry or the
    // DUST = 0 bypass stops being bit-exact.
    daisysp::fonepole(_dt_current, _dt_target, _dt_coef);
    const float ds = _dt_current * _sr;

    if (!_dust.active()) {       // DUST = 0: bit-exact with the pre-DUST path
        l += _echo_l.Process(l * send, ds) * _mix_lin;
        r += _echo_r.Process(r * send, ds) * _mix_lin;
        return;
    }

    // The grain taps read the tape as it stands at the START of this sample —
    // built before Process() advances the write head. Both channels share one
    // write pointer (they are written in lockstep).
    const TapeTap tap{_echo_l.line(), _echo_r.line(), _echo_l.write_ptr(),
                      static_cast<int32_t>(kMaxSamples) - 1};
    float gl = 0.f, gr = 0.f;
    const float wb = _dust.process(tap, gl, gr);

    const float e_l = _echo_l.Process(l * send, ds, wb);
    const float e_r = _echo_r.Process(r * send, ds, wb);

    // Grain sum joins BEFORE _mix_lin: FLUX MIX stays the single wet control
    // for everything coming off the tape. Grain reads deliberately skip the
    // band-pass and tanh — the cloud is rawer and brighter than the echo.
    const float hg = _dust.head_gain();
    l += (e_l * hg + gl) * _mix_lin;
    r += (e_r * hg + gr) * _mix_lin;
}
