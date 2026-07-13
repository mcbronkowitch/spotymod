#pragma once
#include <array>
#include <cstddef>
#include "parts/part.h"
#include "mod/lane_id.h"
#include "fx/reverb.h"
#include "center/center.h"

namespace spky {

enum PartId { PART_A = 0, PART_B = 1, PART_COUNT = 2 };

// FX memory injected by the host (spec "No heap"): echo buffers of
// Flux::kMaxSamples floats each, and storage for the one shared reverb.
// Desktop: static arrays / static object. Daisy (M6): SDRAM.
struct FxMem {
    float* echo[PART_COUNT][2] = { { nullptr, nullptr }, { nullptr, nullptr } };
    AmbientReverb* reverb = nullptr;
};

// The complete public API. No hardware type crosses this boundary; the same
// object is driven by the desktop render host and (later) the firmware shell.
class Instrument {
public:
    void init(float sample_rate);                    // engine only, no FX chain
    void init(float sample_rate, const FxMem& mem);  // full FX chain
    void set_tempo_bpm(float bpm);

    void set_rate(int p, float n)            { _parts[p].mod().set_rate(n); }
    void set_sync_mode(int p, SyncMode m)    { _parts[p].mod().set_sync_mode(m); }
    void set_shape(int p, float n)           { _parts[p].mod().set_shape(n); }
    void set_probability(int p, float n)     { _parts[p].mod().set_probability(n); }
    void set_smooth(int p, float n)          { _parts[p].mod().set_smooth(n); }
    void set_range(int p, float n)           { _parts[p].mod().set_range(n); }
    void set_entropy(int p, float n)         { _parts[p].mod().set_entropy(n); }  // -1..+1
    void set_step(int p, bool on, int steps) { _parts[p].set_step(on, steps); }
    void set_fixed_slew(int p, bool on)      { _parts[p].mod().set_fixed_slew(on); }
    void set_depth(int p, float n)           { _parts[p].set_depth(n); }
    void set_tune(int p, float n)            { _parts[p].set_tune(n); }
    void set_target_active(int p, int s, bool on) { _parts[p].set_target_active(s, on); }
    void set_target_base(int p, int s, float n)   { _parts[p].set_target_base(s, n); }
    void set_target_depth(int p, int s, float n)  { _parts[p].set_target_depth(s, n); }
    void set_scale(int scale_idx) {
        if (scale_idx < 0) scale_idx = 0;
        if (scale_idx >= SCALE_LIST_COUNT) scale_idx = SCALE_LIST_COUNT - 1;
        for (auto& part : _parts) part.quant().set_scale(SCALE_MASKS[scale_idx]);
    }
    void set_quant_mode(int p, QuantMode m) { _parts[p].quant().set_mode(m); }
    void set_root(int p, int semis)         { _parts[p].quant().set_root(semis); }

    void set_fx_on(int p, FxBlock which, bool on)  { _parts[p].fx().set_fx_on(which, on); }
    void set_grit_mode(int p, GritMode m)          { _parts[p].fx().set_grit_mode(m); }
    void set_fx_target_active(int p, int i, bool on) { _parts[p].set_fx_target_active(i, on); }
    void set_fx_target_base(int p, int i, float n) { _parts[p].set_fx_target_base(i, n); }
    void set_fx_target_depth(int p, int i, float n){ _parts[p].set_fx_target_depth(i, n); }
    void set_flux_mix(int p, float n)              { _parts[p].fx().set_flux_mix(n); }
    void set_grit_mix(int p, float n)              { _parts[p].fx().set_grit_mix(n); }
    void set_comp(int p, float n)                  { _parts[p].fx().set_comp(n); }
    void set_reverb_size(float n)  { if (_reverb) _reverb->set_size(n); }
    void set_reverb_decay(float n) { if (_reverb) _reverb->set_decay(n); }
    void set_reverb_tone(float n)  { if (_reverb) _reverb->set_tone(n); }
    void set_reverb_depth(float n) { if (_reverb) _reverb->set_depth(n); }
    float fx_target_value(int p, int i) const { return _parts[p].fx_target_value(i); }

    // --- M2 synth voice API (spec "Instrument API") ---
    void set_engine(int p, EngineId e)       { _parts[p].set_engine(e); }
    void set_voice_attack(int p, float n)    { _parts[p].set_voice_attack(n); }
    void set_voice_decay(int p, float n)     { _parts[p].set_voice_decay(n); }
    void set_voice_resonance(int p, float n) { _parts[p].set_voice_resonance(n); }
    void set_voice_sub(int p, float n)       { _parts[p].set_voice_sub(n); }
    void set_voice_detune(int p, float n)    { _parts[p].set_voice_detune(n); }
    void trigger_manual(int p)               { _parts[p].trigger_manual(); }
    int  active_voices(int p) const          { return _parts[p].active_voices(); }
    float voice_env(int p, int v) const      { return _parts[p].voice_env(v); }
    EngineId engine_id(int p) const          { return _parts[p].engine_id(); }

    float lane_output(int p, int s)  const { return _parts[p].lane_output(s); }
    float target_value(int p, int s) const { return _parts[p].target_value(s); }
    bool  lane_fired(int p, int s)   const { return _parts[p].lane_fired(s); }
    bool  gate(int p)  const { return _parts[p].gate(); }
    float pitch_cv(int p) const { return _parts[p].pitch_cv(); }

    // --- M3 capture sequencer (per part) ---
    void capture_now(int p)         { _parts[p].mod().capture_now(); }
    void set_replay(int p, bool on) { _parts[p].mod().set_replay(on); }
    bool replaying(int p) const     { return _parts[p].mod().replaying(); }
    bool loop_valid(int p) const    { return _parts[p].mod().loop_valid(); }

    // --- M4 center section ---
    void set_morph(float m)  { _center.set_morph(m); }
    void set_couple(float c) { _center.set_couple(c); }
    void set_drift(float d)  { _center.set_drift(d); }
    void spot()   { _center.spot(_parts[PART_A].mod(),   _parts[PART_B].mod()); }
    void settle() { _center.settle(_parts[PART_A].mod(), _parts[PART_B].mod()); }
    float morph()     const { return _center.morph(); }
    float couple()    const { return _center.couple(); }
    float drift()     const { return _center.drift(); }
    float weather()   const { return _center.weather(); }
    float phase_err() const { return _center.phase_err(); }

    void process(const float* inL, const float* inR, float* outL, float* outR, size_t n);

private:
    std::array<Part, PART_COUNT> _parts;
    AmbientReverb* _reverb = nullptr;
    Center _center;
    int    _ctrl_ctr = 0;    // counts down to the next control-rate Center::update
    float _sr = 48000.f;
    float _bpm = 120.f;
};

} // namespace spky
