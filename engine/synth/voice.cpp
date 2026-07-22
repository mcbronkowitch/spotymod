#include "synth/voice.h"
#include <cmath>
#include "mod/rng.h"
#include "util/fast_sin.h"
#include "util/math.h"

using namespace spky;

namespace {
constexpr float kDriftDetuneCt = 3.f;     // micro-detune drift ceiling (spec: +/-3 ct)
constexpr float kDriftPanAmt   = 0.25f;   // pan drift ceiling around the fan slot
} // namespace

void Voice::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _osc_a.init(sample_rate);
    _osc_b.init(sample_rate);
    _env.init(sample_rate);
    _filt.Init(sample_rate);
    _filt.SetFreq(2000.f);
    _filt.SetRes(0.15f);   // no SetDrive: SvfLp has no drive term (svf_lp.h)

    Rng rng;                               // used ONCE at init: deterministic
    rng.seed(seed);                        // per-voice drift character
    _drift_pan_hz    = 0.05f + 0.15f * rng.next_unipolar();   // 0.05..0.2 Hz
    _drift_det_hz    = 0.05f + 0.15f * rng.next_unipolar();
    _drift_pan_phase = rng.next_unipolar();
    _drift_det_phase = rng.next_unipolar();

    _sub_phase = 0.f;
    _drift_ct_cur = 0.f;
    set_pitch_hz(220.f);
    update_control(0.f);
}

void Voice::trigger(float freq_hz) {
    set_pitch_hz(freq_hz);                 // applies to the oscs immediately
    _env.trigger();                        // from current level: click-free steal
}

void Voice::set_sustaining(bool on) { _env.set_sustain(on ? 0.7f : 0.f); }

void Voice::set_vel(float v) {
    _vel_target = clampf(v, 0.f, 1.f);
    if (!_env.active()) _vel = _vel_target;   // idle: snap — nothing can click
}

void Voice::set_pitch_hz(float freq_hz) {
    _freq = freq_hz < 0.f ? 0.f : freq_hz;
    _sub_inc = 0.5f * _freq / _sr;         // sub: one octave below
    _apply_freq();
}

void Voice::set_env_times(float attack_s, float decay_s) {
    _env.set_times(attack_s, decay_s);
}

void Voice::set_morph(float m) {
    _osc_a.set_morph(m);
    _osc_b.set_morph(m);
}

void Voice::set_detune_cents(float max_ct) { _detune_ct = max_ct; }
void Voice::set_sub_level(float n)   { _sub_level = clampf(n, 0.f, 1.f); }
void Voice::set_cutoff_hz(float hz)  { _filt.SetFreq(clampf(hz, 20.f, 0.3f * _sr)); }
void Voice::set_resonance(float n)   { _filt.SetRes(clampf(n, 0.f, 0.95f)); }
void Voice::set_pan(float pan)       { _pan_base = clampf(pan, -1.f, 1.f); }
void Voice::set_drift_amount(float a){ _drift_amt = clampf(a, 0.f, 1.f); }

void Voice::_apply_freq() {                // control-rate (std::pow inside)
    const float half = _detune_ct * 0.5f;
    _osc_a.set_detune_cents(half + _drift_ct_cur);
    _osc_b.set_detune_cents(-half - _drift_ct_cur);
    _osc_a.set_freq(_freq);
    _osc_b.set_freq(_freq);
}

void Voice::update_control(float dt_s) {
    _vel += 0.35f * (_vel_target - _vel);     // ~10 ms at the 96-sample tick (ear-tunable)
    _drift_pan_phase += _drift_pan_hz * dt_s;
    _drift_pan_phase -= std::floor(_drift_pan_phase);
    _drift_det_phase += _drift_det_hz * dt_s;
    _drift_det_phase -= std::floor(_drift_det_phase);

    const float drift_pan = fast_sin(_drift_pan_phase) * kDriftPanAmt * _drift_amt;
    _drift_ct_cur = fast_sin(_drift_det_phase) * kDriftDetuneCt * _drift_amt;
    _apply_freq();

    // equal-power pan: angle 0..0.25 turns; gl = cos, gr = sin (via fast_sin)
    const float pan = clampf(_pan_base + drift_pan, -1.f, 1.f);
    const float a = (pan + 1.f) * 0.125f;
    _gain_r = fast_sin(a);
    _gain_l = fast_sin(a + 0.25f);
}

void Voice::process(float& accL, float& accR) {
    if (!_env.active()) return;            // idle voices are free

    float s = 0.5f * (_osc_a.process() + _osc_b.process());
    _sub_phase += _sub_inc;
    if (_sub_phase >= 1.f) _sub_phase -= 1.f;
    s += _sub_level * fast_sin(_sub_phase);

    _filt.Process(s);
    s = _filt.Low() * _env.process() * _vel;

    accL += s * _gain_l;
    accR += s * _gain_r;
}
