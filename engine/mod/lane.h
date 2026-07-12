#pragma once
#include <cstdint>
#include "util/onepole.h"
#include "mod/rng.h"

namespace spky {

class CaptureLoop;   // engine/mod/capture.h — wired to the PITCH lane only

// One modulation lane: wavetable core -> probability -> step/flow -> smooth
// -> range. Bipolar output in [-1,1]. Deterministic given its seed.
class ModLane {
public:
    void init(float sample_rate, uint32_t seed);

    void set_rate_hz(float hz);
    void set_shape(float s);          // 0..1
    void set_probability(float p);    // 0..1
    void set_step(bool on, int steps_per_cycle);
    void set_fixed_slew(bool on);     // panel switch 3 middle position
    void set_smooth(float s);         // 0..1
    void set_range(float r);          // 0..1
    void set_entropy(float e);        // -1..+1: erode / loop (0) / grow

    float process();                  // advance one sample, return post-range value

    bool  fired()  const { return _fired; }   // true on the sample a boundary fired
    bool  frozen() const { return _frozen; }  // last dice failed -> holding
    float phase()  const { return _phase; }
    float target() const { return _target; }  // pre-smooth, pre-range held value

    void reset(float phase = 0.f);

    // M3 capture: wired once at init on the PITCH lane only (nullptr elsewhere).
    void set_capture_loop(CaptureLoop* loop) { _capture_loop = loop; }
    void set_replay(bool on) { _replay = on; if (on) _play_slot = -1; }
    bool replaying() const { return _replaying(); }

    // --- M4 center hooks ---
    void set_shape_offset(float o) { _shape_offset = o; }  // DRIFT bank-wide shape tap
    void kick(float dphase, float dshape);                 // SPOT: phase jump + decaying shape
    void settle();                                         // panic: glide EVOLVE + kick to 0

private:
    void  _update_slew();
    void  _on_boundary();
    float _compute_raw() const;
    int   _sh_slot() const;         // which _seq slot the S&H end reads now
    void  _mutate_slot(int slot);   // entropy dice + walk/erode on a fired step
    int   _phase_slot() const;      // floor(phase * kSlots), clamped
    void  _record_slot();           // roll _target + fired into the ring
    bool  _replaying() const;       // replay requested AND loop valid
    void  _replay_step();           // loop is the source this sample

    Rng     _rng;
    OnePole _slew;

    float _sr = 48000.f;
    float _phase = 0.f;
    float _phase_inc = 0.f;
    float _shape = 0.f;
    float _prob = 1.f;
    float _range = 1.f;
    float _smooth = 0.f;
    float _entropy = 0.f;

    bool  _step_mode = false;
    int   _steps = 8;
    bool  _fixed_slew = false;

    int   _cur_step = -1;
    static constexpr int kSeqSlots = 32;
    float _seq[kSeqSlots] = {};  // looping S&H step buffer — the melody (spec: entropy sequencer)
    float _target = 0.f;     // pre-smooth held value
    bool  _fired = false;
    bool  _frozen = false;

    float _ev_phase = 0.f;   // EVOLVE random-walk offsets: shape / phase / rate (Task 7)
    float _ev_shape = 0.f;
    float _ev_rate  = 0.f;

    // M4 center hooks
    float _shape_offset = 0.f;   // DRIFT shape tap (set per control tick)
    float _kick_shape   = 0.f;   // SPOT shape offset, decays with _kick_coef
    float _kick_coef    = 1.f;   // per-sample decay for _kick_shape (tau ~ 0.4 s)
    int   _settle_ctr   = 0;     // >0: gliding EVOLVE walks + kick to 0
    float _settle_coef  = 1.f;   // per-sample settle glide (tau ~ 0.3 s)

    // M3 capture (recording state; replay state added in Task 3)
    CaptureLoop* _capture_loop = nullptr;
    int          _rec_slot = -1;    // last ring slot written this pass
    bool         _rec_fired = false;// a boundary has fired since entering _rec_slot
    bool         _replay = false;   // replay requested (effective only if loop valid)
    int          _play_slot = -1;   // last slot evaluated while replaying
};

} // namespace spky
