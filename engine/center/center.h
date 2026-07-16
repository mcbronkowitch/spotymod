#pragma once
#include <cstdint>
#include "parts/part.h"
#include "mod/super_modulator.h"
#include "mod/rng.h"
#include "mod/divisions.h"
#include "util/onepole.h"
#include "util/math.h"
#include "center/transport.h"

namespace spky {

// The center section: one control-rate brain over the two banks. Owns MORPH
// (equal-power A/B gains), COUPLE (Kuramoto PLL), DRIFT (one OU weather walk
// tapped to six destinations), SPOT (per-lane stumble) and SETTLE (panic).
// update() reads both banks' phases/rates and writes back through narrow hooks;
// the audio path only multiplies the morph gains.
class Center {
public:
    static constexpr int kCtrlInterval = 96;   // control tick, matches M2

    void init(float sample_rate, uint32_t seed);

    // performable amounts
    void set_morph(float m)  { _morph_target  = clampf(m, 0.f, 1.f); }
    void set_couple(float c) { _couple        = clampf(c, 0.f, 1.f); }
    void set_drift(float d)  { _drift_target  = clampf(d, 0.f, 1.f); }
    void set_sync(bool on)        { _sync = on; }
    void set_tempo_bpm(float bpm) { _transport.set_bpm(bpm); }
    void clock_pulse()            { _transport.clock_pulse(); }
    void reset_transport()        { _transport.reset(); }
    const Transport& transport() const { return _transport; }

    // one control tick: read both banks, write hooks, advance weather + morph
    void update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb);

    // gestures
    void spot(SuperModulator& a, SuperModulator& b);
    void settle(SuperModulator& a, SuperModulator& b);

    // getters (CSV + later LEDs)
    float gain_a()    const { return _g_a; }
    float gain_b()    const { return _g_b; }
    float morph()     const { return _morph; }
    float couple()    const { return _couple; }
    float drift()     const { return _drift; }
    float weather()   const { return _weather; }
    float phase_err() const { return _phase_err; }

private:
    void _step_weather();
    float _grid_servo(const SuperModulator& m) const;

    Transport _transport;
    bool      _sync = false;

    float _sr = 48000.f;
    float _cr = 500.f;               // control rate = sr / kCtrlInterval

    // MORPH
    OnePole _morph_smooth;
    float   _morph_target = 0.5f;
    float   _morph = 0.5f;
    float   _g_a = 0.70710678f;
    float   _g_b = 0.70710678f;

    // COUPLE
    float _couple = 0.f;
    float _phase_err = 0.f;

    // DRIFT
    OnePole _drift_smooth;
    float   _drift_target = 0.f;
    float   _drift = 0.f;
    float   _ou = 0.f;               // Ornstein-Uhlenbeck state (unbounded)
    float   _weather = 0.f;          // tanh(_ou), softly bounded to (-1,1)
    Rng     _weather_rng;

    // SPOT / SETTLE
    Rng   _spot_rng;
    int   _settle_ctr = 0;           // >0: weather gliding to 0
    float _settle_coef = 1.f;
};

} // namespace spky
