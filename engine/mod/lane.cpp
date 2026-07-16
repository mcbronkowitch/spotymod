#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

// Mutation character — tuned by ear; the spec fixes behavior, not constants.
static constexpr float kGravity  = 0.10f;  // GROW: mild pull toward 0 (the root)

void ModLane::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _rng.seed(seed);
    _phase = 0.f;
    _cur_step = -1;
    if (_melodic) {
        generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);
        pg_gen_groove(_rng, _layout.motif_len, _groove);
    } else {
        _fill_walk();
        for (int i = 0; i < kSeqSlots; ++i) { _gate[i] = true; _motif_id[i] = 0; }
    }
    _regen_pending = false;
    _density = 1.f;
    _target = 0.f;
    _fired = false;
    _frozen = false;
    _note_age = 0;
    _note_hold = 0;
    _ev_phase = 0.f;
    _ev_shape = 0.f;
    _ev_rate  = 0.f;
    _shape_offset = 0.f;
    _kick_shape   = 0.f;
    _kick_coef    = std::exp(-1.f / (1.5f * _sr));   // SPOT shape decay tau = 1.5 s
    _settle_coef  = std::exp(-1.f / (0.3f * _sr));   // SETTLE glide tau = 0.3 s
    _settle_ctr   = 0;
    _update_slew();
    _slew.reset(0.f);
}

float ModLane::phase_eff() const { float p = _phase + _ev_phase; return p - std::floor(p); }

void ModLane::set_rate_hz(float hz)   { _phase_inc = (hz > 0.f ? hz : 0.f) / _sr; }
void ModLane::set_shape(float s)      { _shape = clampf(s, 0.f, 1.f); }
void ModLane::set_range(float r)      { _range = clampf(r, 0.f, 1.f); }
void ModLane::set_variation(float v)  { _variation = clampf(v, -1.f, 1.f); }

void ModLane::set_step(bool on, int steps) {
    _step_mode = on;
    int new_steps = steps < 1 ? 1 : steps;
    if (_melodic) {
        int old_n = _steps > kSeqSlots ? kSeqSlots : _steps;
        int new_n = new_steps > kSeqSlots ? kSeqSlots : new_steps;
        if (new_n != old_n) _regen_pending = true; // only when effective length changes
    }
    _steps = new_steps;
}

void ModLane::new_phrase() { if (_melodic) _regen_pending = true; }

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
    _phase += dphase;
    _phase -= std::floor(_phase);          // permanent wrap into [0,1)
    _kick_shape += dshape;                 // decays back to 0 over ~1.5 s
}

void ModLane::settle() {
    _settle_ctr = static_cast<int>(_sr * 1.0f);   // glide EVOLVE + kick over ~1 s
}

void ModLane::reset(float phase) {
    _phase = clampf(phase, 0.f, 0.999999f);
    _cur_step = -1;
    _note_age = 0;
    _note_hold = 0;
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

int ModLane::_groove_k() const {
    int L = _groove.len < 1 ? 1 : _groove.len;
    int k = static_cast<int>(std::lround(_density * static_cast<float>(L)));
    if (k < 1) k = 1;              // the anchor is unmaskable
    if (k > L) k = L;
    return k;
}

bool ModLane::_effective_gate(int slot) const {
    if (!_melodic) return _gate[slot];   // non-melodic lanes: all-true, DENSE unrouted
    return _groove.rank_of_slot[slot % _groove.len] < _groove_k();
}

void ModLane::_on_boundary() {
    int slot = _sh_slot();
    // STEP consults the effective gate (groove rank vs DENSE); FLOW has no
    // per-step gate so it always fires (no freeze source after PROBABILITY).
    bool gated = _step_mode ? _effective_gate(slot) : true;
    _frozen = !gated;
    if (gated) {
        _fired = true;
        if (_melodic && _step_mode) _start_note(slot);
        if (_variation > 0.f) _mutate_slot(slot);  // GROW pitch
        _target = _compute_raw();
    } else {
        ++_note_age;   // rest step: the running note ages toward its release
    }
    // if !gated: hold the previous _target (frozen) — and the buffer slot with it
}

void ModLane::_start_note(int slot) {
    int n = _steps > kSeqSlots ? kSeqSlots : _steps;   // effective phrase length
    if (n < 1) n = 1;
    int dist = 1;                                       // steps to the next note
    while (dist < n && !_effective_gate((slot + dist) % n)) ++dist;
    int hold = static_cast<int>(_groove.note_len[slot % _groove.len]);
    _note_hold = hold > dist ? dist : hold;             // reaching the next note = tie
    _note_age = 0;
}

void ModLane::_mutate_slot(int slot) {
    // GROW only: dice ∝ variation^2 (squared for fine control near LOOP).
    if (_rng.next_unipolar() >= _variation * _variation) return; // dice ∝ variation²
    float v = _seq[slot];
    // Random walk from the old value. The cubed draw makes small intervals
    // common and leaps rare; width opens with variation; the (1 - kGravity)
    // factor is the tonic gravity keeping lines anchored.
    float r = _rng.next_bipolar();
    float delta = r * r * r * lerpf(0.5f, 2.f, _variation); // cubed: small common
    v = clampf((v + delta) * (1.f - kGravity), -1.f, 1.f);  // mild tonic gravity
    _seq[slot] = v;
}

void ModLane::_fill_walk() {
    pg_contour_walk(_rng, _seq, kSeqSlots, 0.f, 0.6f, 0.12f);
}

void ModLane::_renew_units() {
    int units = _layout.motif_count;                 // number of renewal units
    for (int u = 0; u < units; ++u) {
        if (_rng.next_unipolar() < _variation * _variation)  // per-unit dice
            regenerate_unit(_principle, _rng, _layout, _motif_id, u, _seq, _gate);
    }
}
void ModLane::_renew_walk() {
    pg_contour_walk(_rng, _seq, kSeqSlots, 0.f, 0.6f, 0.12f);
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
    _phase += _phase_inc * (1.f + _ev_rate);
    bool wrapped = false;
    while (_phase >= 1.f) { _phase -= 1.f; wrapped = true; }

    if (wrapped) {
        if (_regen_pending && _melodic && _step_mode) {
            generate_phrase(_principle, _rng, _steps, _seq, _gate, _motif_id, _layout);
            pg_gen_groove(_rng, _layout.motif_len, _groove);
            _regen_pending = false;
            _ev_phase = _ev_shape = _ev_rate = 0.f; // present fresh phrase un-warped
        }
        if (_variation > 0.f) {                 // GROW: EVOLVE contour walk (live)
            _ev_phase = clampf(_ev_phase + _rng.next_bipolar() * 0.01f * _variation, -0.5f, 0.5f);
            _ev_shape = clampf(_ev_shape + _rng.next_bipolar() * 0.02f * _variation, -0.25f, 0.25f);
            _ev_rate  = clampf(_ev_rate  + _rng.next_bipolar() * 0.01f * _variation, -0.2f, 0.2f);
        } else if (_variation < 0.f) {          // RENEW: per-unit regen + walk decay
            if (_melodic && _step_mode) _renew_units();
            else if (!_melodic) {
                if (_rng.next_unipolar() < _variation * _variation) _renew_walk();
            }
            float decay = 1.f + 0.2f * _variation;  // variation -1 -> x0.8/cycle
            _ev_phase *= decay; _ev_shape *= decay; _ev_rate *= decay;
        }                                       // variation 0 (LOOP): walk frozen
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

    float smoothed = _slew.process(_target);
    return apply_range(smoothed, _range);
}
