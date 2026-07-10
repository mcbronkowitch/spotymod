#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

void ModLane::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _rng.seed(seed);
    _phase = 0.f;
    _cur_step = -1;
    _sh_cycle = _rng.next_bipolar();
    _target = 0.f;
    _fired = false;
    _frozen = false;
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _update_slew();
    _slew.reset(0.f);
}

void ModLane::set_rate_hz(float hz)   { _phase_inc = (hz > 0.f ? hz : 0.f) / _sr; }
void ModLane::set_shape(float s)      { _shape = clampf(s, 0.f, 1.f); }
void ModLane::set_probability(float p){ _prob = clampf(p, 0.f, 1.f); }
void ModLane::set_range(float r)      { _range = clampf(r, 0.f, 1.f); }
void ModLane::set_evolve(float a)     { _evolve = clampf(a, 0.f, 1.f); }

void ModLane::set_step(bool on, int steps) {
    _step_mode = on;
    _steps = steps < 1 ? 1 : steps;
}

void ModLane::set_smooth(float s) {
    _smooth = clampf(s, 0.f, 1.f);
    _update_slew();
}

void ModLane::set_fixed_slew(bool on) {
    _fixed_slew = on;
    _update_slew();
}

void ModLane::_update_slew() {
    // smooth 0 -> ~1 sample (near passthrough), smooth 1 -> ~0.5 s.
    float t = _fixed_slew ? 0.02f : (0.00002f * std::pow(25000.f, _smooth));
    _slew.init(_sr, t);
}

void ModLane::reset(float phase) {
    _phase = clampf(phase, 0.f, 0.999999f);
    _cur_step = -1;
    _slew.reset(_target);
}

float ModLane::_compute_raw() const {
    float ph = _phase + _ev_phase;
    ph -= std::floor(ph);
    float sh = clampf(_shape + _ev_shape, 0.f, 1.f);
    return shape_value(ph, sh, _sh_cycle);
}

void ModLane::_on_boundary() {
    bool fire = _rng.next_unipolar() < _prob;
    _frozen = !fire;
    if (fire) {
        _fired = true;
        _target = _compute_raw();   // latch the value at this boundary
    }
    // if !fire: hold the previous _target (frozen)
}

float ModLane::process() {
    _fired = false;

    _phase += _phase_inc * (1.f + _ev_rate);           // EVOLVE also wanders the rate
    bool wrapped = false;
    while (_phase >= 1.f) { _phase -= 1.f; wrapped = true; }

    if (wrapped) {
        _sh_cycle = _rng.next_bipolar();               // new S&H value per cycle
        if (_evolve > 0.f) {                           // EVOLVE: shape / phase / rate random walk (Task 7)
            _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _evolve, -0.5f, 0.5f);
            _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _evolve, -0.25f, 0.25f);
            _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _evolve, -0.2f, 0.2f);
        }
    }

    if (_step_mode) {
        int step = static_cast<int>(_phase * _steps);
        if (step >= _steps) step = _steps - 1;
        if (step != _cur_step) {
            _cur_step = step;
            _on_boundary();
        }
    } else {
        if (wrapped) _on_boundary();
        if (!_frozen) _target = _compute_raw();        // continuous in FLOW
    }

    float smoothed = _slew.process(_target);
    return apply_range(smoothed, _range);
}
