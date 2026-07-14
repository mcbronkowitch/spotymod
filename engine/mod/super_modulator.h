#pragma once
#include <array>
#include <cstdint>
#include "mod/lane.h"
#include "mod/lane_id.h"

namespace spky {

// One performable macro surface driving five independent lanes at fixed
// musical ratios of the master RATE. The PITCH lane leads (rate x1).
class SuperModulator {
public:
    void init(float sample_rate, uint32_t seed_base);

    void set_tempo_bpm(float bpm)  { _bpm = bpm; _update_rate(); }
    void set_rate(float norm)      { _rate_norm = norm; _update_rate(); }
    void set_sync_mode(SyncMode m) { _mode = m; _update_rate(); }
    void set_shape(float s);
    void set_probability(float p);
    void set_smooth(float s);
    void set_range(float r);
    void set_entropy(float a);
    void set_step(bool on, int steps);
    void set_fixed_slew(bool on);
    void set_principle(Principle p) { _lanes[LANE_PITCH].set_principle(p); }

    void process();                // advance all lanes one sample

    float lane_output(int i) const { return _out[i]; }
    bool  lane_fired(int i)  const { return _lanes[i].fired(); }
    bool  lane_frozen(int i) const { return _lanes[i].frozen(); }
    float lane_phase(int i)  const { return _lanes[i].phase(); }
    float pitch_phase()      const { return _lanes[LANE_PITCH].phase(); }
    float master_hz()        const { return _master_hz; }

    // --- M4 center hooks ---
    void set_rate_scale(float s)  { _rate_scale = s; _apply_rate(); }  // COUPLE * DRIFT rate
    void set_shape_offset(float o){ for (auto& l : _lanes) l.set_shape_offset(o); }
    void spot(Rng& rng);          // per-lane SPOT kicks (skips the PITCH lane)
    void settle()                { for (auto& l : _lanes) l.settle(); }
    float    base_hz()   const { return _base_hz; }   // rate before COUPLE/DRIFT scale
    SyncMode sync_mode() const { return _mode; }

private:
    void _update_rate();
    void _apply_rate();

    std::array<ModLane, LANE_COUNT> _lanes;
    std::array<float, LANE_COUNT>   _out {};

    float    _sr = 48000.f;
    float    _bpm = 120.f;
    float    _rate_norm = 0.5f;
    SyncMode _mode = SyncMode::Free;
    float    _master_hz = 1.f;
    float    _base_hz    = 1.f;   // rate from knob/sync, before rate_scale
    float    _rate_scale = 1.f;   // COUPLE * DRIFT rate multiplier
};

} // namespace spky
