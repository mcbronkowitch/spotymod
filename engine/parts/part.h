#pragma once
#include <array>
#include <cstdint>
#include "mod/super_modulator.h"
#include "pitch/quantizer.h"
#include "parts/engine_iface.h"
#include "parts/test_tone_engine.h"
#include "util/math.h"

namespace spky {

// A part = SuperModulator + selectable engine + 5 targets. Combines each lane's
// bipolar output with a stored per-target base + depth, gated by the target's
// active flag and the master DEPTH.
class Part {
public:
    void init(float sample_rate, uint32_t seed_base);

    SuperModulator& mod() { return _mod; }
    Quantizer& quant() { return _quant; }

    void set_depth(float d) { _depth = clampf(d, 0.f, 1.f); }
    void set_tune(float t)  { _tune = clampf(t, 0.f, 1.f); }
    void set_target_active(int slot, bool on) { _active[slot] = on; }
    void set_target_base(int slot, float b)   { _base[slot] = clampf(b, 0.f, 1.f); }
    void set_target_depth(int slot, float d)  { _tdepth[slot] = clampf(d, 0.f, 1.f); }

    float target_value(int slot) const;
    float target_raw(int slot) const;          // base + mod*depth, unquantized
    float pitch_pre_quant() const;             // PITCH target + TUNE, pre-quantize
    float lane_output(int slot) const { return _mod.lane_output(slot); }
    bool  lane_fired(int slot) const  { return _mod.lane_fired(slot); }
    bool  gate() const { return _gate_ctr > 0; }
    float pitch_cv() const { return target_value(LANE_PITCH); }

    void process(float& outL, float& outR);    // advance mod one sample + engine

private:
    SuperModulator _mod;
    TestToneEngine _tone;
    IPartEngine*   _engine = nullptr;

    std::array<bool,  LANE_COUNT> _active { { false, false, true, false, true } };
    std::array<float, LANE_COUNT> _base   { { 0.5f, 0.5f, 0.5f, 0.5f, 0.8f } };
    std::array<float, LANE_COUNT> _tdepth { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    float _depth = 1.f;
    float _tune = 0.5f;
    int   _gate_ctr = 0;
    int   _gate_len = 240;   // ~5 ms @ 48k, recomputed in init()
    float _sr = 48000.f;

    Quantizer _quant;
    float     _pitch_q = 0.f;
};

} // namespace spky
