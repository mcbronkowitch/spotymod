#pragma once
#include <array>
#include <cstddef>
#include "parts/part.h"
#include "mod/lane_id.h"
#include "fx/reverb.h"
#include "fx/limiter.h"
#include "center/center.h"
#include "util/onepole.h"

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
    void set_density(int p, float d)         { _parts[p].mod().set_density(d); }
    void set_smooth(int p, float n)          { _parts[p].mod().set_smooth(n); }
    void set_range(int p, float n)           { _parts[p].mod().set_range(n); }
    void set_variation(int p, float n)       { _parts[p].mod().set_variation(n); }  // -1..+1
    void set_principle(int p, int pr)        { _parts[p].mod().set_principle(static_cast<Principle>(pr)); }
    void set_step(int p, bool on, int steps) { _parts[p].set_step(on, steps); }
    void new_phrase(int p)                   { _parts[p].mod().new_phrase(); }
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
    void set_reverb_diffusion(float n) { if (_reverb) _reverb->set_diffusion(n); }
    void set_reverb_smear(float n) { if (_reverb) _reverb->set_diffuser_mod_depth(n); }
    void set_reverb_mod(float n)   { if (_reverb) _reverb->set_mod_depth(n); }
    void set_reverb_mix(float n);   // 0..1 equal-power dry/wet at the master join
    void set_master_drive(float n) { _limiter.set_drive(n); }
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
    bool  pitch_gate(int p) const { return _parts[p].mod().pitch_gate(); }

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
    bool reverb_asleep() const { return _rev_asleep; }

    void process(const float* inL, const float* inR, float* outL, float* outR, size_t n);

private:
    std::array<Part, PART_COUNT> _parts;
    AmbientReverb* _reverb = nullptr;
    float   _rev_dry_target = 1.f;  // equal-power gain targets (exact endpoints)
    float   _rev_wet_target = 0.f;
    OnePole _rev_dry, _rev_wet;     // 10 ms glide at the master join
    bool    _rev_primed = false;    // first process() snaps the mix gains
    bool    _rev_asleep = false;    // MIX 0 gate: room cleared, process() skipped
    Limiter _limiter;
    Center _center;
    int    _ctrl_ctr = 0;    // counts down to the next control-rate Center::update
    float _sr = 48000.f;
    float _bpm = 120.f;
};

} // namespace spky
