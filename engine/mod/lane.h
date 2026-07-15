#pragma once
#include <cstdint>
#include "util/onepole.h"
#include "mod/rng.h"
#include "mod/phrase_gen.h"

namespace spky {

// One modulation lane: wavetable core -> gate (note/rest x density) -> step/flow
// -> smooth -> range. Bipolar output in [-1,1]. Deterministic given its seed.
class ModLane {
public:
    void init(float sample_rate, uint32_t seed);

    void set_rate_hz(float hz);
    void set_shape(float s);          // 0..1
    void set_density(float d) { _density = pg_clampf(d, 0.f, 1.f); }  // 0..1
    void set_step(bool on, int steps_per_cycle);
    void set_fixed_slew(bool on);     // panel switch 3 middle position
    void set_smooth(float s);         // 0..1
    void set_range(float r);          // 0..1
    void set_variation(float v);      // -1..+1: renew / loop (0) / grow

    void set_melodic(bool m) { _melodic = m; }
    void set_principle(Principle p) { _principle = p; }
    void new_phrase();                 // audition a fresh phrase at the next STEP-mode wrap

    float process();                  // advance one sample, return post-range value

    bool  fired()  const { return _fired; }   // true on the sample a boundary fired
    bool  frozen() const { return _frozen; }  // last dice failed -> holding
    bool  gate_state() const { return _step_mode ? _effective_gate(_sh_slot()) : true; }
    float phase()  const { return _phase; }
    float phase_eff() const;                  // audible phase = (_phase + EVOLVE offset), wrapped
    float target() const { return _target; }  // pre-smooth, pre-range held value

    void reset(float phase = 0.f);

    // --- M4 center hooks ---
    void set_shape_offset(float o) { _shape_offset = o; }  // DRIFT bank-wide shape tap
    void kick(float dphase, float dshape);                 // SPOT: phase jump + decaying shape
    void settle();                                         // panic: glide EVOLVE + kick to 0

private:
    void  _update_slew();
    void  _on_boundary();
    float _compute_raw() const;
    int   _sh_slot() const;         // which _seq slot the S&H end reads now
    void  _mutate_slot(int slot);   // GROW: variation dice + pitch walk on a fired step
    void  _fill_walk();             // deterministic contour-walk prefill (non-melodic lanes)
    bool  _effective_gate(int slot) const;  // note/rest gate AND density mask
    bool  _density_pass(int slot) const;    // metric-weight threshold from DENSITY
    void  _renew_units();           // RENEW (melodic/STEP): per-unit dice regeneration
    void  _renew_walk();            // RENEW (non-melodic): dice-gated whole-walk regen

    Rng     _rng;
    OnePole _slew;

    float _sr = 48000.f;
    float _phase = 0.f;
    float _phase_inc = 0.f;
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
    Principle _principle = Principle::TwoMotif;
    bool      _melodic   = false;
    float     _density   = 1.f;
    bool      _regen_pending = false;
    float _target = 0.f;     // pre-smooth held value
    bool  _fired = false;
    bool  _frozen = false;

    float _ev_phase = 0.f;   // EVOLVE random-walk offsets: shape / phase / rate (Task 7)
    float _ev_shape = 0.f;
    float _ev_rate  = 0.f;

    // M4 center hooks
    float _shape_offset = 0.f;   // DRIFT shape tap (set per control tick)
    float _kick_shape   = 0.f;   // SPOT shape offset, decays with _kick_coef
    float _kick_coef    = 1.f;   // per-sample decay for _kick_shape (tau ~ 1.5 s)
    int   _settle_ctr   = 0;     // >0: gliding EVOLVE walks + kick to 0
    float _settle_coef  = 1.f;   // per-sample settle glide (tau ~ 0.3 s)
};

} // namespace spky
