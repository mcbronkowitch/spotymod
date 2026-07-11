#include "fx/flux.h"
#include "util/math.h"

using namespace spky;

namespace {
inline float dbfs2lin(float db) { return daisysp::pow10f(db * 0.05f); }
}

void Flux::init(float sample_rate, float* buf_l, float* buf_r) {
    _sw.init(sample_rate);
    _buf_ok = (buf_l != nullptr && buf_r != nullptr);
    if (!_buf_ok) return;
    _echo_l.Init(sample_rate, buf_l);
    _echo_r.Init(sample_rate, buf_r);
    _echo_l.SetLagTime(0.5f);   // the original tape slew
    _echo_r.SetLagTime(0.5f);
    set_time(0.4f, true);       // boot defaults; PartFx drives them afterwards
    set_feedback(0.45f);
    set_mix(0.5f);
}

void Flux::set_time(float norm, bool immediate) {
    if (!_buf_ok) return;
    float t = daisysp::fmap(clampf(norm, 0.f, 1.f), 0.05f, 5.f,
                            daisysp::Mapping::LOG);
    _echo_l.SetDelayTime(t, immediate);
    _echo_r.SetDelayTime(t, immediate);
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
