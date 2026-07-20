#include "mod/lane.h"
#include "mod/waveforms.h"
#include "mod/range.h"
#include "util/math.h"
#include <cmath>

using namespace spky;

// Mutation character — tuned by ear; the spec fixes behavior, not constants.
static constexpr float kGravity  = 0.10f;  // GROW: mild pull toward 0 (the root)
static constexpr float kGrooveVarStart   = 0.25f;  // |variation| below: melody only
static constexpr float kGrooveRerollGate = 0.9f;   // RENEW near the stop may re-roll all
static constexpr float kGrooveRerollProb = 0.25f;  // ...with this chance, when the dice hits

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
    _kick_coef_tick   = std::pow(_kick_coef,   static_cast<float>(kTickInterval));
    _settle_coef_tick = std::pow(_settle_coef, static_cast<float>(kTickInterval));
    _settle_ctr   = 0;
    _update_slew();
    _slew.reset(0.f);
    _slew_tick.reset(0.f);
}

float ModLane::phase_eff() const { float p = _phase + _ev_phase; return p - std::floor(p); }

void ModLane::set_rate_hz(float hz)   { _rate_hz = hz > 0.f ? hz : 0.f; _update_inc(); }
void ModLane::set_shape(float s)      { _shape = clampf(s, 0.f, 1.f); }
void ModLane::set_range(float r)      { _range = clampf(r, 0.f, 1.f); }
void ModLane::set_variation(float v)  { _variation = clampf(v, -1.f, 1.f); }

void ModLane::set_step(bool on, int steps) {
    if (on && !_step_mode) { _note_age = 0; _note_hold = 0; }  // STEP entry: no stale sustain
    int new_steps = steps < 1 ? 1 : steps;
    if (_melodic) {
        int old_n = _steps > kSeqSlots ? kSeqSlots : _steps;
        int new_n = new_steps > kSeqSlots ? kSeqSlots : new_steps;
        if (new_n != old_n) _regen_pending = true; // only when effective length changes
    }
    if (on && _step_mode && new_steps != _steps && _cur_step >= 0) {
        // Seamless live STEPS turn (spec: step-clock): keep the step index and
        // the fraction inside it so the boundary grid never jumps; _cur_step
        // follows along so the next sample sees no ghost boundary. The
        // _cur_step >= 0 guard keeps pre-run configuration (init -> set_step
        // before the first process()) on the old path, where the first sample
        // must still fire step 0.
        // _cur_step is derived from the stored _phase (assigned first) so it
        // can never disagree with the next process()'s
        // static_cast<int>(_phase * _steps) by an ulp; a same-tick SPOT
        // kick() + STEPS turn is absorbed into this rescale (accepted trade).
        float pos = std::fmod(_phase * static_cast<float>(_steps),
                              static_cast<float>(new_steps));
        _phase = pos / static_cast<float>(new_steps);
        _cur_step = static_cast<int>(_phase * static_cast<float>(new_steps));
    }
    _step_mode = on;
    _steps = new_steps;
    _update_inc();
}

// Spec 2026-07-17 step-clock: STEP runs RATE as a step clock with an 8-step
// reference; FLOW keeps RATE as the cycle rate. At 8 steps the factor is
// exactly 1.0f, so the panel default stays bit-identical to the old
// pattern-clock behavior.
void ModLane::_update_inc() {
    _phase_inc = (_rate_hz / _sr) * clock_scale();
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
    // Tick twin: the exact kTickInterval-sample compound of the per-sample
    // coefficient, so held segments converge identically at tick sampling.
    // The `k` derivation + clamp below intentionally mirrors OnePole::init's
    // own coefficient formula (engine/util/onepole.h) so that compounding it
    // kTickInterval times reproduces the per-sample coefficient's effect
    // exactly. If that formula changes, this must change with it -- these
    // two are a matched pair, not independent code, and this tick twin would
    // otherwise silently diverge from process()'s slew.
    float k = 1.f / (t * _sr);
    if (k > 1.f) k = 1.f;
    _slew_tick.set_coef(1.f - std::pow(1.f - k, static_cast<float>(kTickInterval)));
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
    _since_onset = 0;
    _onsets = 0;
    _gap[0] = _gap[1] = 0;
    _rhythm = RhythmView{};
    _slew.reset(_target);
    _slew_tick.reset(_target);
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
        _gap[1] = _gap[0];
        _gap[0] = _since_onset;
        _since_onset = 0;
        if (_onsets < 3) ++_onsets;
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
    // For _steps > kSeqSlots this scan models the 32-slot buffer's own order
    // (slot after step _steps-1 wraps to slot 0), not the outer cycle seam —
    // unreachable from the panel, where STEPS clamps to 16.
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

void ModLane::_mutate_groove(bool renew_side) {
    if (!_melodic || !_step_mode) return;
    float a = _variation < 0.f ? -_variation : _variation;
    float r = (a - kGrooveVarStart) / (1.f - kGrooveVarStart);
    if (r < 0.f) r = 0.f;
    if (r > 1.f) r = 1.f;
    // Dice always drawn while this side is active: fixed base draw count per
    // wrap; in zone 1 (r == 0) it can never pass.
    if (_rng.next_unipolar() >= r * r) return;
    if (renew_side) {
        bool reroll = a >= kGrooveRerollGate && _rng.next_unipolar() < kGrooveRerollProb;
        pg_groove_mutate_renew(_rng, _groove, reroll);
    } else {
        pg_groove_mutate_grow(_rng, _groove);
    }
}

// Cycle-wrap events, shared by process() and tick(): pending phrase regen,
// the EVOLVE walk (GROW) or walk decay + per-unit regen (RENEW), and the
// outer-zone groove mutations. Order is load-bearing and identical to the
// old inline block.
void ModLane::_wrap_events() {
    // Publish the rhythm once per cycle, at the pattern boundary. Between
    // wraps the ring keeps recording but nothing downstream moves -- that is
    // what makes a looping source pattern produce a STANDING tap figure
    // rather than one that rotates on every onset.
    _rhythm.gap[0] = _gap[0];
    _rhythm.gap[1] = _gap[1];
    _rhythm.valid  = _onsets >= 3;
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
        _mutate_groove(false);              // outer zone: rhythm drifts too
    } else if (_variation < 0.f) {          // RENEW: per-unit regen + walk decay
        if (_melodic && _step_mode) _renew_units();
        else if (!_melodic) {
            if (_rng.next_unipolar() < _variation * _variation) _renew_walk();
        }
        float decay = 1.f + 0.2f * _variation;  // variation -1 -> x0.8/cycle
        _ev_phase *= decay; _ev_shape *= decay; _ev_rate *= decay;
        _mutate_groove(true);               // outer zone: re-decide pushes
    }                                       // variation 0 (LOOP): walk frozen
}

float ModLane::process() {
    _fired = false;
    if (_since_onset < kSinceOnsetMax) ++_since_onset;
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

    if (wrapped) _wrap_events();

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

// Advance exactly kTickInterval samples in one call -- the texture-lane path
// (spec 2026-07-19 mod-plane-control-rate). Mirrors process()'s observable
// sequence: every edge (step boundary or wrap) inside the interval runs in
// phase order with identical RNG draws, note aging and mutations; wrap
// events run at their phase position (before the new cycle's step 0); only
// the last target is visible. Boundary targets are evaluated at the grid
// phase (step/steps, resp. 0 at a wrap) instead of the per-sample path's
// detection overshoot (< 1 sample of phase) -- an equally valid sampling of
// the same waveform, covered by the equivalence suite.
float ModLane::tick() {
    _fired = false;
    if (_since_onset < kSinceOnsetMax) _since_onset += kTickInterval;
    _kick_shape *= _kick_coef_tick;
    if (_settle_ctr > 0) {
        // Clamp-to-0 (rather than letting the counter go negative) plus the
        // fact every decayed quantity here targets zero means a mid-window
        // _settle_ctr expiry is harmless: at most it decays one extra partial
        // window's worth on values that are gliding to 0 anyway, never past
        // it and never in the wrong direction. This also means _settle_ctr
        // is NOT guaranteed to be an exact multiple of kTickInterval at
        // every supported sample rate (e.g. 44.1 kHz, as used by Rack) --
        // the clamp is what makes that safe rather than an off-by-one bug.
        _settle_ctr = _settle_ctr > kTickInterval ? _settle_ctr - kTickInterval : 0;
        _ev_phase   *= _settle_coef_tick;
        _ev_shape   *= _settle_coef_tick;
        _ev_rate    *= _settle_coef_tick;
        _kick_shape *= _settle_coef_tick;
    }

    // Pending step mismatch first: init/reset leave _cur_step = -1 and the
    // per-sample path fires step 0 on its very first sample the same way.
    // This same check also absorbs kick()'s phase jumps (a kick can land
    // _phase past the current step's boundary without _cur_step having
    // moved), FLOW->STEP re-entry (_cur_step is stale from before FLOW was
    // engaged), and float overshoot of the final partial-interval phase
    // advance (rounding can nudge _phase a hair past a step edge the walk
    // below already accounted for). Do not simplify this to an init/reset-
    // only check -- all four cases share the same "phase says a different
    // step than _cur_step remembers" symptom and this one branch catches
    // them all.
    if (_step_mode) {
        int step = static_cast<int>(_phase * static_cast<float>(_steps));
        if (step >= _steps) step = _steps - 1;
        if (step != _cur_step) { _cur_step = step; _on_boundary(); }
    }

    // Walk every edge inside the interval, in order. Panel-reachable worst
    // case is ~8 edges (480 Hz effective STEP rate, ~12.5 samples/step); the
    // cap is a safety bound, unreachable from the panel (spec: 2*kSeqSlots).
    float samples_left = static_cast<float>(kTickInterval);
    int guard = 2 * kSeqSlots;
    while (guard-- > 0) {
        // _ev_rate can change at a wrap (GROW walk), so the per-sample rate
        // is re-derived per edge -- the per-sample path does the same.
        const float dp1 = _phase_inc * (1.f + _ev_rate);
        const float next_edge = _step_mode
            ? static_cast<float>(_cur_step + 1) / static_cast<float>(_steps)
            : 1.f;
        const float dist = next_edge - _phase;
        const float to_edge = dp1 > 0.f ? dist / dp1 : 1e30f;
        if (to_edge > samples_left) { _phase += samples_left * dp1; break; }
        samples_left -= to_edge;
        if (next_edge >= 1.f) {
            _phase = 0.f;
            _wrap_events();
            if (_step_mode) _cur_step = 0;
            _on_boundary();              // FLOW fires per wrap; STEP fires step 0
        } else {
            _phase = next_edge;
            ++_cur_step;
            _on_boundary();
        }
    }

    if (!_step_mode && !_frozen) _target = _compute_raw();   // continuous FLOW

    float smoothed = _slew_tick.process(_target);
    return apply_range(smoothed, _range);
}
