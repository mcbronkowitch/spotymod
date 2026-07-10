#pragma once
#include <array>
#include <cstddef>
#include "parts/part.h"
#include "mod/lane_id.h"

namespace spky {

enum PartId { PART_A = 0, PART_B = 1, PART_COUNT = 2 };

// The complete public API. No hardware type crosses this boundary; the same
// object is driven by the desktop render host and (later) the firmware shell.
class Instrument {
public:
    void init(float sample_rate);
    void set_tempo_bpm(float bpm);

    void set_rate(int p, float n)            { _parts[p].mod().set_rate(n); }
    void set_sync_mode(int p, SyncMode m)    { _parts[p].mod().set_sync_mode(m); }
    void set_shape(int p, float n)           { _parts[p].mod().set_shape(n); }
    void set_probability(int p, float n)     { _parts[p].mod().set_probability(n); }
    void set_smooth(int p, float n)          { _parts[p].mod().set_smooth(n); }
    void set_range(int p, float n)           { _parts[p].mod().set_range(n); }
    void set_evolve(int p, float n)          { _parts[p].mod().set_evolve(n); }
    void set_step(int p, bool on, int steps) { _parts[p].mod().set_step(on, steps); }
    void set_fixed_slew(int p, bool on)      { _parts[p].mod().set_fixed_slew(on); }
    void set_depth(int p, float n)           { _parts[p].set_depth(n); }
    void set_tune(int p, float n)            { _parts[p].set_tune(n); }
    void set_target_active(int p, int s, bool on) { _parts[p].set_target_active(s, on); }
    void set_target_base(int p, int s, float n)   { _parts[p].set_target_base(s, n); }
    void set_target_depth(int p, int s, float n)  { _parts[p].set_target_depth(s, n); }

    float lane_output(int p, int s)  const { return _parts[p].lane_output(s); }
    float target_value(int p, int s) const { return _parts[p].target_value(s); }
    bool  lane_fired(int p, int s)   const { return _parts[p].lane_fired(s); }
    bool  gate(int p)  const { return _parts[p].gate(); }
    float pitch_cv(int p) const { return _parts[p].pitch_cv(); }

    void process(const float* inL, const float* inR, float* outL, float* outR, size_t n);

private:
    std::array<Part, PART_COUNT> _parts;
    float _sr = 48000.f;
    float _bpm = 120.f;
};

} // namespace spky
