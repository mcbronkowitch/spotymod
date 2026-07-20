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
    // Guarded at the source, not at each reader: bpm() feeds a divide in
    // every consumer (nearest_division()/division_hz() for COUPLE's grid
    // gravity, and the transport's own beat_phase()/beats() readers), so a
    // single non-positive value stored here would otherwise reach all of
    // them as a non-finite result. A non-positive or non-finite
    // request is dropped and the last good tempo (default 120) is kept,
    // rather than clamped to some arbitrary floor BPM: scenario files forward
    // their `bpm` field unvalidated (host/render/scenario.cpp), and a
    // zero/negative value there is bad input, not a real slow tempo to honor.
    void set_bpm(float bpm) { if (bpm > 0.f && std::isfinite(bpm)) _bpm = bpm; }
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
