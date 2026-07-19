#pragma once
#include <array>
#include <cstdint>
#include "mod/lane.h"
#include "mod/lane_id.h"
#include "mod/divisions.h"

namespace spky {

// One performable macro surface driving five independent lanes at fixed
// musical ratios of the master RATE. The PITCH lane leads (rate x1).
class SuperModulator {
public:
    void init(float sample_rate, uint32_t seed_base);

    void set_tempo_bpm(float bpm)  { _bpm = bpm; _update_rate(); }
    void set_rate(float norm)      { _rate_norm = norm; _update_rate(); }
    void set_synced(bool on)       { _synced = on; _update_tide(); _update_rate(); }
    void set_tide(float norm);     // 0..1 texture-lane rate scale; 0.5 = neutral
    float tide_mult() const { return _tide_mult; }
    void set_shape(float s);
    void set_density(float d) { _lanes[LANE_PITCH].set_density(d); }
    void set_smooth(float s);
    void set_range(float r);
    void set_variation(float v);
    void set_step(bool on, int steps);
    void set_fixed_slew(bool on);
    void set_principle(Principle p) { _lanes[LANE_PITCH].set_principle(p); }
    void new_phrase() { _lanes[LANE_PITCH].new_phrase(); }
    bool pitch_gate() const { return _lanes[LANE_PITCH].gate_state(); }
    bool pitch_sustain() const { return _lanes[LANE_PITCH].note_sustain(); }

    void process();                // advance all lanes one sample

    float lane_output(int i) const { return _out[i]; }
    bool  lane_fired(int i)  const { return _lanes[i].fired(); }
    bool  lane_frozen(int i) const { return _lanes[i].frozen(); }
    float lane_phase(int i)  const { return _lanes[i].phase(); }
    float pitch_phase()      const { return _lanes[LANE_PITCH].phase(); }
    float pitch_phase_eff()  const { return _lanes[LANE_PITCH].phase_eff(); }  // audible pitch phase
    float master_hz()        const { return _master_hz; }

    // --- M4 center hooks ---
    void set_rate_scale(float pitch_s, float mod_s) {
        _pitch_scale = pitch_s; _mod_scale = mod_s; _apply_rate();
    }
    float pitch_scale() const { return _pitch_scale; }
    float mod_scale()   const { return _mod_scale; }
    void set_shape_offset(float o){ for (auto& l : _lanes) l.set_shape_offset(o); }
    void spot(Rng& rng);          // per-lane SPOT kicks (skips the PITCH lane)
    void settle()                { for (auto& l : _lanes) l.settle(); }
    // RST bar resync: all lanes restart at phase 0 (step 0 fires on the next
    // sample), so the loops land ON the fresh downbeat instead of being
    // dragged onto it by the grid servo.
    void reset_phases()          { for (auto& l : _lanes) l.reset(0.f); }
    float base_hz()   const { return _base_hz; }   // rate before COUPLE/DRIFT scale
    bool  synced()    const { return _synced; }
    int   division()  const { return division_index(_rate_norm); }
    // Step-clock factor of the pitch/master lane (8/steps in STEP, 1 in FLOW):
    // the grid servo scales its transport target by this (spec 2026-07-17).
    float clock_scale() const { return _lanes[LANE_PITCH].clock_scale(); }

private:
    void _update_rate();
    void _apply_rate();
    void _update_tide();

    std::array<ModLane, LANE_COUNT> _lanes;
    std::array<float, LANE_COUNT>   _out {};

    float    _sr = 48000.f;
    float    _bpm = 120.f;
    float    _rate_norm = 0.5f;
    bool     _synced = false;
    float    _pitch_scale = 1.f;   // COUPLE/DRIFT on the melody clock
    float    _mod_scale   = 1.f;   // COUPLE/DRIFT on the texture lanes
    float    _master_hz = 1.f;
    float    _base_hz    = 1.f;   // rate from knob/sync, before rate_scale
    float    _tide_norm = 0.5f;   // TIDE knob position (0..1)
    float    _tide_mult = 1.f;    // effective factor: ladder rung or free curve
    int      _tick_ctr = 0;        // texture-lane raster; 0 = tick on next process()
};

} // namespace spky
