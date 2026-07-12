#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "mod/capture.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

// Mutation character — tuned by ear; the spec fixes behavior, not constants.
static constexpr float kGravity  = 0.10f;  // GROW: mild pull toward 0 (the root)
static constexpr float kErode    = 0.60f;  // ERODE: fraction kept per mutation
static constexpr float kRootSnap = 0.02f;  // ERODE: below this, land exactly on 0

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
    _shape_offset = 0.f;
    _kick_shape   = 0.f;
    _kick_coef    = std::exp(-1.f / (1.5f * _sr));   // SPOT shape decay tau = 1.5 s
    _settle_coef  = std::exp(-1.f / (0.3f * _sr));   // SETTLE glide tau = 0.3 s
    _settle_ctr   = 0;
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

void ModLane::kick(float dphase, float dshape) {
    if (_replaying()) return;              // captured loop never mutates (M3/M4 guard)
    _phase += dphase;
    _phase -= std::floor(_phase);          // permanent wrap into [0,1)
    _kick_shape += dshape;                 // decays back to 0 over ~0.4 s
}

void ModLane::settle() {
    _settle_ctr = static_cast<int>(_sr * 1.0f);   // glide EVOLVE + kick over ~1 s
}

void ModLane::reset(float phase) {
    _phase = clampf(phase, 0.f, 0.999999f);
    _cur_step = -1;
    _slew.reset(_target);
}

float ModLane::_compute_raw() const {
    float ph = _phase + _ev_phase;
    ph -= std::floor(ph);
    float sh = clampf(_shape + _ev_shape + _shape_offset + _kick_shape, 0.f, 1.f);
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
        if (_entropy != 0.f) _mutate_slot(_sh_slot());  // fired steps only: held
        _target = _compute_raw();   // latch the value at this boundary
    }
    // if !fire: hold the previous _target (frozen) — and the buffer slot with it
}

void ModLane::_mutate_slot(int slot) {
    // Dice: mutation chance grows with |entropy|; squared for fine control near LOOP.
    if (_rng.next_unipolar() >= _entropy * _entropy) return;
    float v = _seq[slot];
    if (_entropy > 0.f) {
        // GROW: random walk from the old value. The cubed draw makes small
        // intervals common and leaps rare; width opens with entropy; the
        // (1 - kGravity) factor is the tonic gravity keeping lines anchored.
        float r = _rng.next_bipolar();
        float delta = r * r * r * lerpf(0.5f, 2.f, _entropy);
        v = clampf((v + delta) * (1.f - kGravity), -1.f, 1.f);
    } else {
        // ERODE: pull the note toward 0 (root / base value); snap when close
        // so sustained erosion lands exactly on a single repeated root note.
        v *= kErode;
        if (std::fabs(v) < kRootSnap) v = 0.f;
    }
    _seq[slot] = v;
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
    _kick_shape *= _kick_coef;                 // SPOT shape offset fades toward 0
    if (_settle_ctr > 0) {                     // SETTLE: glide EVOLVE walks + kick to 0
        --_settle_ctr;
        _ev_phase   *= _settle_coef;
        _ev_shape   *= _settle_coef;
        _ev_rate    *= _settle_coef;
        _kick_shape *= _settle_coef;
    }
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
