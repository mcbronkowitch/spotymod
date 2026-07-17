#pragma once
#include <cstdint>
#include "Filters/svf.h"
#include "synth/morph_osc.h"
#include "synth/env.h"

namespace spky {

// One synth voice (x4 per part):
//
//   MorphOsc A ─┐
//   MorphOsc B ─┼→ mix → Svf lowpass (FILTER) → Env (VCA) → equal-power pan
//   sub sine  ──┘
//
// plus a slow per-voice drift LFO pair (pan + micro-detune, ~0.05-0.2 Hz,
// rates drawn deterministically from spky::Rng at init). All parameters
// arrive from SynthEngine at CONTROL RATE (update_control, once per
// 96-sample block); process() is the pure per-sample audio path and uses
// fast_sin only. daisysp::Svf is the single DaisySP dependency (spec).
class Voice {
public:
    void init(float sample_rate, uint32_t seed);

    void trigger(float freq_hz);          // latch pitch + retrigger env from level
    void set_sustaining(bool on);         // FLOW: sustain 0.7; off = AD / demotion
    void set_pitch_hz(float freq_hz);     // FLOW sustaining voice tracks the target
    void set_vel(float v);                // chord gain comp; slewed, snaps idle

    // control-rate parameter feeds
    void set_env_times(float attack_s, float decay_s);
    void set_morph(float m);
    void set_detune_cents(float max_ct);  // split +/- max_ct/2 across osc A/B
    void set_sub_level(float n);
    void set_cutoff_hz(float hz);
    void set_resonance(float n);
    void set_pan(float pan);              // base pan -1..1 (MOTION fan slot)
    void set_drift_amount(float a);       // 0..1 (proportional to MOTION width)
    void update_control(float dt_s);      // advance drift, refresh freqs + gains

    void process(float& accL, float& accR);   // adds into the accumulators

    bool  active() const { return _env.active(); }
    float env_value() const { return _env.value(); }

private:
    void _apply_freq();                   // osc A/B freq from pitch+detune+drift

    MorphOsc _osc_a;
    MorphOsc _osc_b;
    Env _env;
    daisysp::Svf _filt;

    float _sr = 48000.f;
    float _freq = 220.f;
    float _sub_phase = 0.f;
    float _sub_inc = 0.f;
    float _sub_level = 0.3f;
    float _detune_ct = 0.f;               // TIMBRE-spread detune (max, split half)
    float _pan_base = 0.f;
    float _drift_amt = 0.f;
    float _gain_l = 0.70710678f;
    float _gain_r = 0.70710678f;
    float _vel = 1.f;
    float _vel_target = 1.f;

    // slow deterministic drift (control-rate)
    float _drift_pan_phase = 0.f;
    float _drift_det_phase = 0.f;
    float _drift_pan_hz = 0.1f;
    float _drift_det_hz = 0.1f;
    float _drift_ct_cur = 0.f;            // current micro-detune drift (cents)
};

} // namespace spky
