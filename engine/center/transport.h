#pragma once
#include <cmath>

namespace spky {

// Master transport: a running beat counter advanced at the Center's control
// rate. The host reports external clock edges (one pulse per beat) through
// clock_pulse(); RST zeroes the downbeat. The accumulator is double — float
// loses beat-phase precision within minutes at 500 ticks/s.
class Transport {
public:
    void init(float ctrl_rate) { _cr = ctrl_rate; _beats = 0.0; }
    void set_bpm(float bpm)    { _bpm = bpm; }
    float bpm() const          { return _bpm; }

    void tick()        { _beats += static_cast<double>(_bpm) / (60.0 * static_cast<double>(_cr)); }
    void clock_pulse() { _beats = std::round(_beats); }   // snap to the nearest beat
    void reset()       { _beats = 0.0; }

    double beats() const     { return _beats; }
    float beat_phase() const { return static_cast<float>(_beats - std::floor(_beats)); }

private:
    double _beats = 0.0;
    float  _bpm = 120.f;
    float  _cr  = 500.f;
};

} // namespace spky
