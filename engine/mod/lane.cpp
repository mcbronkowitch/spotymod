#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "mod/capture.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

void ModLane::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _rng.seed(seed);
    _phase = 0.f;
    _cur_step = -1;
    for (float& v : _seq) v = _rng.next_bipolar();   // a melody exists from cycle one
    _target = 0.f;
    _fired = false;
    _frozen = false;
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _rec_slot = -1;
    _rec_fired = false;
    _replay = false;
    _play_slot = -1;
    _update_slew();
    _slew.reset(0.f);
}

void ModLane::set_rate_hz(float hz)   { _phase_inc = (hz > 0.f ? hz : 0.f) / _sr; }
void ModLane::set_shape(float s)      { _shape = clampf(s, 0.f, 1.f); }
void ModLane::set_probability(float p){ _prob = clampf(p, 0.f, 1.f); }
void ModLane::set_range(float r)      { _range = clampf(r, 0.f, 1.f); }
void ModLane::set_entropy(float e)    { _entropy = clampf(e, -1.f, 1.f); }

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
    return shape_value(ph, sh, _seq[_sh_slot()]);
}

int ModLane::_sh_slot() const {
    if (!_step_mode) return 0;                 // FLOW: one slot, loop-stable per cycle
    int s = _cur_step < 0 ? 0 : _cur_step;
    return s % kSeqSlots;
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

int ModLane::_phase_slot() const {
    int s = static_cast<int>(_phase * CaptureLoop::kSlots);
    return s >= CaptureLoop::kSlots ? CaptureLoop::kSlots - 1 : s;
}

void ModLane::_record_slot() {
    int slot = _phase_slot();
    if (slot != _rec_slot) { _rec_fired = false; _rec_slot = slot; } // new slot: clear
    if (_fired) _rec_fired = true;                                   // latch a fire
    _capture_loop->record(slot, _target, _rec_fired);
}

bool ModLane::_replaying() const {
    return _replay && _capture_loop && _capture_loop->valid();
}

void ModLane::_replay_step() {
    int slot = _phase_slot();
    if (slot != _play_slot) {
        _play_slot = slot;
        if (_capture_loop->fired(slot)) {
            bool fire = _rng.next_unipolar() < _prob;  // live PROBABILITY dice
            _frozen = !fire;
            if (fire) _fired = true;                   // trigger; freeze lifts
        }
    }
    if (!_frozen) _target = _capture_loop->value(slot); // curve, or held step
}

float ModLane::process() {
    _fired = false;
    const bool replay = _replaying();

    _phase += _phase_inc * (replay ? 1.f : (1.f + _ev_rate));  // no EVOLVE rate on replay
    bool wrapped = false;
    while (_phase >= 1.f) { _phase -= 1.f; wrapped = true; }

    if (replay) {
        _replay_step();                                 // the loop is the source
    } else {
        if (wrapped) {
            if (_entropy > 0.f) {                       // GROW: EVOLVE random walk (live only)
                _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _entropy, -0.5f, 0.5f);
                _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _entropy, -0.25f, 0.25f);
                _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _entropy, -0.2f, 0.2f);
            } else if (_entropy < 0.f) {                // ERODE: walk settles toward neutral
                float decay = 1.f + 0.2f * _entropy;    // entropy -1 -> x0.8 per cycle
                _ev_phase *= decay;
                _ev_shape *= decay;
                _ev_rate  *= decay;
            }                                           // entropy 0 (LOOP): walk frozen
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
            if (!_frozen) _target = _compute_raw();     // continuous in FLOW
        }

        if (_capture_loop) _record_slot();              // roll into the ring
    }

    float smoothed = _slew.process(_target);
    return apply_range(smoothed, _range);
}
