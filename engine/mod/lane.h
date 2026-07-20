#pragma once
#include <cstdint>
#include "util/onepole.h"
#include "mod/rng.h"
#include "mod/phrase_gen.h"

namespace spky {

// One modulation lane: wavetable core -> gate (groove rank x DENSE) -> step/flow
// -> smooth -> range. Bipolar output in [-1,1]. Deterministic given its seed.
class ModLane {
public:
    // Control-raster interval of the tick() path (spec 2026-07-19
    // mod-plane-control-rate). part.cpp static_asserts this against
    // SynthEngine::kCtrlInterval -- the mod layer must not include synth.
    static constexpr int kTickInterval = 96;

    void init(float sample_rate, uint32_t seed);

    void set_rate_hz(float hz);
    void set_shape(float s);          // 0..1
    void set_density(float d) { _density = pg_clampf(d, 0.f, 1.f); }  // 0..1 -> how deep into the groove ranking (k of L cell notes)
    void set_step(bool on, int steps);
    void set_fixed_slew(bool on);     // panel switch 3 middle position
    void set_smooth(float s);         // 0..1
    void set_range(float r);          // 0..1
    void set_variation(float v);      // -1..+1: renew / loop (0) / grow

    void set_melodic(bool m) { _melodic = m; }
    void set_principle(Principle p) { _principle = p; }
    void new_phrase();                 // audition a fresh phrase at the next STEP-mode wrap

    float process();                  // advance one sample, return post-range value
    float tick();                     // advance kTickInterval samples in one call

    bool  fired()   const { return _fired; }    // true on the sample a gated boundary fired
    // True on the sample the cycle phase wrapped (0 <= _phase < 1 crossing
    // back through 0). Distinct from fired(): a STEP lane can fire a
    // boundary on every step without wrapping, and a wrap in FLOW mode fires
    // exactly one boundary, so the two coincide there but not in STEP. Exists
    // so SuperModulator can maintain the onset-gap ring for LANE_PITCH
    // without ModLane itself carrying that state -- see super_modulator.cpp.
    bool  wrapped() const { return _wrapped; }
    bool  frozen() const { return _frozen; }  // last dice failed -> holding
    // GATE: melodic STEP sustains the composed note (age < hold); else high.
    bool  gate_state() const { return _step_mode ? (!_melodic || _note_age < _note_hold) : true; }
    // Step-mode-qualified sustain: true only while a melodic STEP note holds.
    // Unlike gate_state() this is false in FLOW and non-melodic lanes, so it
    // is safe to OR into a pulse-based gate without forcing it permanently high.
    bool  note_sustain() const { return _step_mode && _melodic && _note_age < _note_hold; }
    float phase()  const { return _phase; }
    // Step-clock factor on the cycle rate (spec 2026-07-17): 8/steps in STEP,
    // 1 in FLOW. The grid servo scales its transport target by this so a
    // synced bank locks its S-step cycle across S/8 divisions.
    float clock_scale() const { return _step_mode ? 8.f / static_cast<float>(_steps) : 1.f; }
    float phase_eff() const;                  // audible phase = (_phase + EVOLVE offset), wrapped
    float target() const { return _target; }  // pre-smooth, pre-range held value

    void reset(float phase = 0.f);

    // --- M4 center hooks ---
    void set_shape_offset(float o) { _shape_offset = o; }  // DRIFT bank-wide shape tap
    void kick(float dphase, float dshape);                 // SPOT: phase jump + decaying shape
    void settle();                                         // panic: glide EVOLVE + kick to 0

private:
    void  _update_slew();
    void  _update_inc();            // step-clock: inc = rate/sr * (STEP ? 8/steps : 1)
    void  _on_boundary();
    void  _wrap_events();           // regen/EVOLVE/groove events at a cycle wrap
    float _compute_raw() const;
    int   _sh_slot() const;         // which _seq slot the S&H end reads now
    void  _mutate_slot(int slot);   // GROW: variation dice + pitch walk on a fired step
    void  _fill_walk();             // deterministic contour-walk prefill (non-melodic lanes)
    bool  _effective_gate(int slot) const;  // melodic: groove rank < DENSE depth; else all-true
    int   _groove_k() const;              // DENSE -> how many ranked cell notes play
    void  _renew_units();           // RENEW (melodic/STEP): per-unit dice regeneration
    void  _renew_walk();            // RENEW (non-melodic): dice-gated whole-walk regen
    void  _mutate_groove(bool renew_side);  // VARIATION outer zone: rhythm dice (wrap only)
    void  _start_note(int slot);    // groove: set _note_hold (tie-capped) on fire

    Rng     _rng;
    OnePole _slew;
    OnePole _slew_tick;          // tick-rate twin of _slew; a lane is driven by
                                 // exactly ONE path, so the twin's state never
                                 // fights the per-sample instance

    float _sr = 48000.f;
    float _phase = 0.f;
    float _phase_inc = 0.f;
    float _rate_hz = 0.f;
    float _shape = 0.f;
    float _range = 1.f;
    float _smooth = 0.f;
    float _variation = 0.f;

    bool  _step_mode = false;
    int   _steps = 8;
    bool  _fixed_slew = false;

    int   _cur_step = -1;
    static constexpr int kSeqSlots = 32;
    float _seq[kSeqSlots] = {};  // looping S&H step buffer — the melody (spec: entropy sequencer)
    bool      _gate[kSeqSlots]      = {};
    uint8_t   _motif_id[kSeqSlots]  = {};
    PhraseLayout _layout;
    GrooveCell _groove;
    Principle _principle = Principle::TwoMotif;
    bool      _melodic   = false;
    float     _density   = 1.f;
    bool      _regen_pending = false;
    float _target = 0.f;     // pre-smooth held value
    bool  _fired = false;
    bool  _wrapped = false;
    bool  _frozen = false;
    int   _note_age  = 0;    // steps since the current note fired
    int   _note_hold = 0;    // composed note length (capped at the next note)

    float _ev_phase = 0.f;   // EVOLVE random-walk offsets: shape / phase / rate (Task 7)
    float _ev_shape = 0.f;
    float _ev_rate  = 0.f;

    // M4 center hooks
    float _shape_offset = 0.f;   // DRIFT shape tap (set per control tick)
    float _kick_shape   = 0.f;   // SPOT shape offset, decays with _kick_coef
    float _kick_coef    = 1.f;   // per-sample decay for _kick_shape (tau ~ 1.5 s)
    int   _settle_ctr   = 0;     // >0: gliding EVOLVE walks + kick to 0
    float _settle_coef  = 1.f;   // per-sample settle glide (tau ~ 0.3 s)
    float _kick_coef_tick   = 1.f;   // _kick_coef ^ kTickInterval
    float _settle_coef_tick = 1.f;   // _settle_coef ^ kTickInterval
};

} // namespace spky
