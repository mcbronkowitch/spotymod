#pragma once
#include <array>
#include <cstdint>
#include "mod/super_modulator.h"
#include "pitch/quantizer.h"
#include "pitch/chord.h"
#include "parts/engine_iface.h"
#include "parts/test_tone_engine.h"
#include "synth/synth_engine.h"
#include "sampler/sampler_engine.h"
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
              float* echo_l = nullptr, float* echo_r = nullptr,
              SampleBuffer::Frame* sampler_mem = nullptr, size_t sampler_frames = 0);

    SuperModulator& mod() { return _mod; }
    const SuperModulator& mod() const { return _mod; }
    Quantizer& quant() { return _quant; }
    PartFx& fx() { return _fx; }

    void set_depth(float d) { _depth = clampf(d, 0.f, 1.f); }
    void set_tune(float t)  { _tune = clampf(t, 0.f, 1.f); }
    // COLOR (spec 2026-07-17 chord-layer): 0 = single note (bit-identical),
    // sweeps to a 4-note diatonic chord. Live on the FLOW surface.
    // The knob is stored, not pushed: process() combines it with the MOTION
    // lane and hands the ChordBuilder the effective color (spec 2026-07-18
    // color-motion-target).
    void set_color(float c) { _color = clampf(c, 0.f, 1.f); }
    int  chord_size() const { return _chord.size(); }
    // The color actually handed to the ChordBuilder: the knob plus MOTION's
    // swing (spec 2026-07-18 color-motion-target). Equals the knob when
    // MOD = 0 or the MOTION target is inactive.
    float color_eff() const { return _color_eff; }
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

    // VOICE edit layer - forwarded to BOTH engines directly, so edits stick
    // whichever engine is active. The sampler reinterprets each knob as its
    // cloud analogue (spec: "no dead knobs"), so one panel row serves both.
    void set_voice_attack(float n)    { _synth.set_attack(n);    _sampler.set_window_attack(n); }
    void set_voice_decay(float n)     { _synth.set_decay(n);     _sampler.set_window_decay(n); }
    void set_voice_resonance(float n) { _synth.set_resonance(n); _sampler.set_resonance(n); }
    void set_voice_sub(float n)       { _synth.set_sub(n);       _sampler.set_sub(n); }
    void set_voice_detune(float n)    { _synth.set_detune(n);    _sampler.set_detune(n); }
    void set_voice_filt(float t)      { _synth.set_filt(t);      _sampler.set_filt(t); }

    SamplerEngine& sampler() { return _sampler; }
    const SamplerEngine& sampler() const { return _sampler; }

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
    // The input-carrying form is the real one; the two legacy overloads feed
    // silence, which keeps every existing caller and test bit-identical.
    void process(float inL, float inR, float& outL, float& outR,
                 float& sendL, float& sendR);
    void process(float& outL, float& outR, float& sendL, float& sendR) {
        process(0.f, 0.f, outL, outR, sendL, sendR);
    }
    void process(float& outL, float& outR) {
        float sl, sr;
        process(0.f, 0.f, outL, outR, sl, sr);
    }

private:
    SuperModulator _mod;
    TestToneEngine _tone;
    IPartEngine*   _engine = nullptr;
    SynthEngine    _synth;
    SamplerEngine  _sampler;
    bool           _last_gate = false;
    SoftSwitch     _engine_fade;
    EngineId       _engine_id = ENGINE_SYNTH;
    EngineId       _pending_engine = ENGINE_SYNTH;
    bool           _switching = false;
    bool           _step_on = false;
    bool           _inhibit = false;
    bool           _note_suppressed = false;   // last fire was swallowed
    float          _last_master_hz = -1.f;

    IPartEngine* _engine_for(EngineId e) {
        switch (e) {
            case ENGINE_SYNTH:   return static_cast<IPartEngine*>(&_synth);
            case ENGINE_SAMPLER: return static_cast<IPartEngine*>(&_sampler);
            default:             return static_cast<IPartEngine*>(&_tone);
        }
    }

    // Everything the engine and FX consume at their own control rate -- see
    // the doc comment on the definition in part.cpp for the full contract
    // (what it does, why it is not idempotent, and how it stays
    // phase-aligned with SynthEngine's own control tick).
    void _control_tick();

    // Control raster. Both this counter and SynthEngine::_ctrl_ctr init to 0
    // and advance once per call to their respective process(), so while both
    // run continuously they fire on the same samples (0, kCtrlInterval,
    // 2*kCtrlInterval, ...), and _control_tick() runs before _engine->
    // process() within that sample -- the order the per-sample code
    // delivered.
    //
    // set_engine() re-arms this counter to 0 on the sample its swap
    // completes (see the swap block in process()), so that sample is always
    // a raster tick: the freshly active engine gets set_targets()/
    // set_chord() then and there, not up to kCtrlInterval - 1 samples later
    // on its power-on defaults. A side effect worth knowing: if the swap
    // lands off the sample-0-based grid (0, kCtrlInterval, 2*kCtrlInterval,
    // ...), this counter's own future ticks permanently rebase to (swap
    // sample, swap sample + kCtrlInterval, ...) instead -- harmless for the
    // engine (nothing downstream cares which absolute grid the raster is
    // on), but it means code or tests that assume ticks land on the
    // original sample-0 grid need the swap to land on it too, or to compute
    // "on raster" relative to the swap sample instead.
    //
    // That re-arm does NOT re-align SynthEngine::_ctrl_ctr. That counter
    // only advances inside SynthEngine::process() (synth_engine.cpp), so it
    // freezes for as long as SynthEngine is the inactive engine and resumes
    // counting from the frozen value once swapped back in -- a *permanent*
    // phase offset against this counter, (samples SynthEngine was inactive)
    // mod kCtrlInterval, not a one-interval blip, and it does not self-heal.
    // set_targets()/set_chord() still land immediately (they write
    // _targets[]/_chord[] directly), but anything SynthEngine only
    // recomputes inside its own _update_control() -- cutoff, resonance,
    // pan/drift width, detune, attack/decay times, the live chord-surface
    // bloom/collapse -- keeps updating on that offset schedule, not on this
    // raster, until the next swap changes the offset again. The SoftSwitch
    // fade does not mask this: it is a 192-sample (4 ms) rise, so across one
    // kCtrlInterval window it reaches only ~0.5 gain, not zero.
    //
    // This alignment claim is about SynthEngine specifically -- TestToneEngine
    // has no control tick at all and reads t[LANE_PITCH] every sample, so
    // under this raster its pitch becomes a kCtrlInterval-sample staircase
    // too. That is acceptable (it is a diagnostic engine, not the audio
    // path), just not "aligned" in the sense the rest of this comment
    // describes.
    int _ctrl_ctr = 0;

    // Target cache: _control_tick() both fills it and pushes it to the
    // engine via set_targets() -- process() no longer pushes it itself, it
    // only reads _tg[LANE_PITCH] back out for the fire's chord build. Boot
    // values mirror SynthEngine::_targets so a push before the first tick
    // cannot hand the engine garbage.
    float _tg[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };

    // FX target cache, filled at the control tick. PartFx smooths each value
    // over 2 ms, so the raster's steps never reach an FX parameter raw.
    // Boot values mirror _fx_base so the first block cannot push zeros.
    float _fxv[FXT_COUNT] = { 0.3f, 0.4f, 1.f, 0.25f, 0.45f };

    PartFx         _fx;

    // Modulation first is the shipped state (spec 2026-07-17 boot-targets):
    // all five targets boot active, with staggered depths — FILTER 0.55 (the
    // exponential cutoff dominates; the big sweep belongs to FILT), MOTION
    // 0.7 (width moves without pumping). Ear-tunable. M6 pads toggle _active.
    std::array<bool,  LANE_COUNT> _active { { true, true, true, true, true } };
    std::array<float, LANE_COUNT> _base   { { 0.5f, 0.5f, 0.5f, 0.5f, 0.8f } };
    std::array<float, LANE_COUNT> _tdepth { { 1.f, 0.55f, 1.f, 0.7f, 1.f } };

    // FX target row (boot: all modulation inactive, spec "Boot defaults").
    // Bases, by FxTargetId: GRIT_INT .3 | FLUX_TIME .4 | FX_MIX 1 | REV_SEND .25 | FLUX_FB .45
    std::array<bool,  FXT_COUNT> _fx_active { { false, false, false, false, false } };
    std::array<float, FXT_COUNT> _fx_base   { { 0.3f, 0.4f, 1.f, 0.25f, 0.45f } };
    std::array<float, FXT_COUNT> _fx_depth  { { 1.f, 1.f, 1.f, 1.f, 1.f } };

    // Modulation may duck LEVEL to at most this fraction of its base — the
    // part breathes, it never vanishes (play-test rev 2026-07-17, ear-tunable).
    static constexpr float kLevelFloor = 0.4f;

    // COLOR is a third destination of the MOTION lane (spec 2026-07-18
    // color-motion-target): density pendles +/-1 zone around the knob, so a
    // phrase's stabs differ in size. Bipolar and ADDITIVE, so the reach stays
    // constant across the knob range and a barely-open knob can still rise
    // into chord territory. Both ear-tunable.
    //   kColorMod  swing amplitude at MOD = 1; the zones are 0.25 wide, so
    //              +/-0.2 crosses at most one edge in each direction.
    //   kColorGate knob travel over which the swing fades in. Below it the
    //              swing is scaled toward 0, so COLOR = 0 is structurally
    //              silent (multiplied by zero, not special-cased) and the
    //              chord layer's bit-identity guarantee survives untouched.
    static constexpr float kColorMod  = 0.2f;
    static constexpr float kColorGate = 0.01f;

    float _depth = 1.f;
    float _tune = 0.5f;
    float _detune_cents = 0.f;   // DRIFT detune, applied post-quantizer to the engine only
    float _color = 0.f;          // COLOR knob; effective color is computed in process()
    float _color_eff = 0.f;      // knob + MOTION swing, as last pushed to _chord
    int   _gate_ctr = 0;
    int   _gate_len = 240;   // ~5 ms @ 48k, recomputed in init()
    float _sr = 48000.f;

    Quantizer _quant;
    float     _pitch_q = 0.f;
    ChordBuilder _chord;
    uint16_t _chord_mask() const {
        return _quant.mode() == QuantMode::Chrom ? CHROM_MASK : _quant.scale_mask();
    }
};

} // namespace spky
