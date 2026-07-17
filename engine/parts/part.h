#pragma once
#include <array>
#include <cstdint>
#include "mod/super_modulator.h"
#include "pitch/quantizer.h"
#include "parts/engine_iface.h"
#include "parts/test_tone_engine.h"
#include "synth/synth_engine.h"
#include "fx/fx_util.h"
#include "fx/part_fx.h"
#include "util/math.h"

namespace spky {

// A part = SuperModulator + selectable engine + 5 targets. Combines each lane's
// bipolar output with a stored per-target base + depth, gated by the target's
// active flag and the master DEPTH.
class Part {
public:
    void init(float sample_rate, uint32_t seed_base,
              float* echo_l = nullptr, float* echo_r = nullptr);

    SuperModulator& mod() { return _mod; }
    const SuperModulator& mod() const { return _mod; }
    Quantizer& quant() { return _quant; }
    PartFx& fx() { return _fx; }

    void set_depth(float d) { _depth = clampf(d, 0.f, 1.f); }
    void set_tune(float t)  { _tune = clampf(t, 0.f, 1.f); }
    void set_detune_cents(float c) { _detune_cents = c; }   // DRIFT tune tap; engine pitch only
    void set_target_active(int slot, bool on) { _active[slot] = on; }
    void set_target_base(int slot, float b)   { _base[slot] = clampf(b, 0.f, 1.f); }
    void set_target_depth(int slot, float d)  { _tdepth[slot] = clampf(d, 0.f, 1.f); }

    void set_fx_target_active(int slot, bool on) { _fx_active[slot] = on; }
    void set_fx_target_base(int slot, float b)   { _fx_base[slot] = clampf(b, 0.f, 1.f); }
    void set_fx_target_depth(int slot, float d)  { _fx_depth[slot] = clampf(d, 0.f, 1.f); }
    float fx_target_value(int slot) const;

    // --- engine selection (M2). Boot default: ENGINE_SYNTH. ---
    // Click-free: 4 ms SoftSwitch fade-out -> swap -> 4 ms fade-in; the swap
    // and state re-forwarding happen inside process() at the idle point.
    void set_engine(EngineId e);
    EngineId engine_id() const { return _engine_id; }

    // STEP/FLOW reaches both the lanes and the engine (drone rule)
    void set_step(bool on, int steps);
    bool flow() const { return !_step_on; }   // CHOKE: a FLOW drone is always "on"

    // PLAY tap (M6 wires the gesture; the engine sees an ordinary trigger)
    void trigger_manual();

    // CHOKE (spec 2026-07-16 choke-priority): while inhibited, a lane fire
    // produces no engine trigger and no gate pulse; the suppressed note's
    // STEP sustain is latched out of gate() too. trigger_manual() is a user
    // gesture and is deliberately NOT inhibited.
    void set_inhibit(bool on) {
        if (on == _inhibit) return;
        _inhibit = on;
        _engine->set_hold(on);   // FLOW drone ducks out / fades back in
    }
    float max_voice_env() const;   // 0 when idle or on the test-tone engine

    // VOICE edit layer - forwarded to the synth engine directly, so edits
    // stick even while the test tone is the active engine
    void set_voice_attack(float n)    { _synth.set_attack(n); }
    void set_voice_decay(float n)     { _synth.set_decay(n); }
    void set_voice_resonance(float n) { _synth.set_resonance(n); }
    void set_voice_sub(float n)       { _synth.set_sub(n); }
    void set_voice_detune(float n)    { _synth.set_detune(n); }
    void set_voice_filt(float t)      { _synth.set_filt(t); }

    int active_voices() const {
        return _engine_id == ENGINE_SYNTH ? _synth.active_voices() : 0;
    }
    float voice_env(int v) const {
        return _engine_id == ENGINE_SYNTH ? _synth.voice_env(v) : 0.f;
    }

    float target_value(int slot) const;
    float target_raw(int slot) const;          // base + mod*depth, unquantized
    float pitch_pre_quant() const;             // PITCH target + TUNE, pre-quantize
    float lane_output(int slot) const { return _mod.lane_output(slot); }
    bool  lane_fired(int slot) const  { return _mod.lane_fired(slot); }
    // GATE jack: the ~5 ms retrigger pulse, OR'd with the composed melodic
    // STEP note sustain (spec: rhythm-groove-design.md section 3). pitch_sustain()
    // is step-mode-qualified (false in FLOW), so FLOW stays pulse-only.
    bool  gate() const {
        return _gate_ctr > 0 || (!_note_suppressed && _mod.pitch_sustain());
    }
    float pitch_cv() const { return target_value(LANE_PITCH); }

    // advance mod one sample + engine + part FX; sends = post-FX x REVERB SEND
    void process(float& outL, float& outR, float& sendL, float& sendR);
    void process(float& outL, float& outR) {
        float sl, sr;
        process(outL, outR, sl, sr);
    }

private:
    SuperModulator _mod;
    TestToneEngine _tone;
    IPartEngine*   _engine = nullptr;
    SynthEngine    _synth;
    SoftSwitch     _engine_fade;
    EngineId       _engine_id = ENGINE_SYNTH;
    EngineId       _pending_engine = ENGINE_SYNTH;
    bool           _switching = false;
    bool           _step_on = false;
    bool           _inhibit = false;
    bool           _note_suppressed = false;   // last fire was swallowed
    float          _last_master_hz = -1.f;

    IPartEngine* _engine_for(EngineId e) {
        return e == ENGINE_SYNTH ? static_cast<IPartEngine*>(&_synth)
                                 : static_cast<IPartEngine*>(&_tone);
    }
    PartFx         _fx;

    std::array<bool,  LANE_COUNT> _active { { false, false, true, false, true } };
    std::array<float, LANE_COUNT> _base   { { 0.5f, 0.5f, 0.5f, 0.5f, 0.8f } };
    std::array<float, LANE_COUNT> _tdepth { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    // FX target row (boot: all modulation inactive, spec "Boot defaults").
    // Bases, by FxTargetId: GRIT_INT .3 | FLUX_TIME .4 | FX_MIX 1 | REV_SEND .25 | FLUX_FB .45
    std::array<bool,  FXT_COUNT> _fx_active { { false, false, false, false, false } };
    std::array<float, FXT_COUNT> _fx_base   { { 0.3f, 0.4f, 1.f, 0.25f, 0.45f } };
    std::array<float, FXT_COUNT> _fx_depth  { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    // Modulation may duck LEVEL to at most this fraction of its base — the
    // part breathes, it never vanishes (play-test rev 2026-07-17, ear-tunable).
    static constexpr float kLevelFloor = 0.4f;

    float _depth = 1.f;
    float _tune = 0.5f;
    float _detune_cents = 0.f;   // DRIFT detune, applied post-quantizer to the engine only
    int   _gate_ctr = 0;
    int   _gate_len = 240;   // ~5 ms @ 48k, recomputed in init()
    float _sr = 48000.f;

    Quantizer _quant;
    float     _pitch_q = 0.f;
};

} // namespace spky
