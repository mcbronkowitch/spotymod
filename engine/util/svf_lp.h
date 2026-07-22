#pragma once
#include <cmath>

namespace spky {

// Low-pass-only Chamberlin SVF: daisysp::Svf with everything the engine never
// reads taken out. `Low()` is BIT-IDENTICAL to daisysp::Svf::Low() for every
// call sequence this engine makes; nothing here re-tunes, re-scales or
// re-shapes the filter.
//
// Why it exists (2026-07-22 CPU hunt, measured on the Daisy Seed). Both users
// of the old filter -- Voice (synth/voice.h) and SamplerEngine's stereo pair
// (sampler/sampler_engine.h) -- call SetDrive(0.f) once at init and then read
// ONLY Low(). Counted exactly on the desktop against bench setup_inst_worst,
// the two-part instrument runs 768 Svf::Process calls per 96-sample block, and
// each of those was computing five outputs and a cubic drive term to throw
// four and a half of them away:
//
//   - out_high_/out_band_/out_peak_/out_notch_ are dead stores. Nothing in
//     engine/**, host/** or tests/** ever calls High(), Band(), Peak() or
//     Notch(). Two passes x four outputs = eight multiplies, one subtract and
//     eight member stores per sample per filter, all unread.
//   - `- drive_ * band_ * band_ * band_` is dead arithmetic. drive_ is
//     pre_drive_ * res_, pre_drive_ comes only from SetDrive, and both call
//     sites pass 0.f exactly once and never again -- so drive_ is identically
//     0.f for the whole run and the term subtracts exactly zero. Three
//     multiplies and a subtract per pass, twice per sample.
//   - powf(res_, 0.25f) was recomputed on EVERY SetFreq, although it depends
//     on res_ alone. The synth pushes a modulated cutoff and a static
//     resonance at every control tick, so that powf ran 8x per block purely to
//     produce the number it produced last tick. It is cached here instead.
//     (SetRes still recomputes it -- that is where res_ actually changes.)
//
// The recurrence itself, the coefficient formulas, the two-pass double
// sampling and the 0.5x averaging are copied unchanged from
// lib/DaisySP/Source/Filters/svf.cpp (Copyright 2020 Electrosmith / Emilie
// Gillet, MIT -- see THIRD_PARTY.md). Keep them that way: the point of this
// class is that it is the same filter, not a better one.
//
// If a future feature needs the band-pass, high-pass, notch or peak output, or
// a non-zero drive, do NOT add it back here one member at a time -- go back to
// daisysp::Svf, which is still vendored, and pay for what you use.
class SvfLp {
public:
    void Init(float sample_rate) {
        _sr    = sample_rate;
        _fc    = 200.0f;
        _res   = 0.5f;
        _freq  = 0.25f;
        _damp  = 0.0f;
        _low   = 0.0f;
        _band  = 0.0f;
        _out_low = 0.0f;
        _fc_max  = _sr / 3.f;
        _res_damp = 2.0f * (1.0f - powf(_res, 0.25f));   // cache; see SetFreq
        _fc_req  = -1.f;   // guards: force the first Set* through
        _res_req = -1.f;
    }

    void SetFreq(float f) {
        if (f == _fc_req) return;   // exact guard; sinf is not cheap
        _fc_req = f;
        _fc = fclamp_(f, 1.0e-6f, _fc_max);
        // fs*2 because double sampled -- daisysp::Svf::SetFreq, verbatim
        _freq = 2.0f * sinf(PI_F_ * min_(0.25f, _fc / (_sr * 2.0f)));
        _recalc_damp();
    }

    void SetRes(float r) {
        // The synth pushes one static RESONANCE value at every voice at every
        // control tick (8 calls per block on the two-part instrument), so this
        // guard removes 8 powf per block outright. Exact: same input, same damp.
        if (r == _res_req) return;
        _res_req = r;
        const float res = fclamp_(r, 0.f, 1.f);
        _res = res;
        _res_damp = 2.0f * (1.0f - powf(_res, 0.25f));
        _recalc_damp();
    }

    void Process(float in) {
        // first pass
        float notch = in - _damp * _band;
        _low  = _low + _freq * _band;
        float high = notch - _low;
        _band = _freq * high + _band;
        _out_low = 0.5f * _low;
        // second pass
        notch = in - _damp * _band;
        _low  = _low + _freq * _band;
        high  = notch - _low;
        _band = _freq * high + _band;
        _out_low += 0.5f * _low;
    }

    float Low() const { return _out_low; }

private:
    static constexpr float PI_F_ = 3.1415927410125732421875f;   // == DaisySP PI_F
    static float min_(float a, float b) { return a < b ? a : b; }
    static float fclamp_(float in, float lo, float hi) {
        return fminf(fmaxf(in, lo), hi);
    }

    void _recalc_damp() {
        _damp = min_(_res_damp, min_(2.0f, 2.0f / _freq - _freq * 0.5f));
    }

    float _sr = 48000.f;
    float _fc = 200.f;
    float _fc_max = 16000.f;
    float _res = 0.5f;
    float _fc_req = -1.f;    // last SetFreq/SetRes argument (change guards)
    float _res_req = -1.f;
    float _res_damp = 0.f;
    float _freq = 0.25f;
    float _damp = 0.f;
    float _low = 0.f;
    float _band = 0.f;
    float _out_low = 0.f;
};

}  // namespace spky
