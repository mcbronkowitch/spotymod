#pragma once
#include <array>
#include <cstdint>
#include "parts/engine_iface.h"
#include "synth/voice.h"
#include "util/onepole.h"

namespace spky {

// The M2 polyphonic part engine: 4 trigger-driven voices behind IPartEngine.
//
// - Allocation: round-robin over free voices; none free -> steal the OLDEST
//   (by trigger order); the steal retriggers the envelope from its current
//   level (click-free, Voice::trigger).
// - STEP (flow == false): plain AD - notes end, silence is legitimate.
// - FLOW (flow == true): the most recently triggered voice is the SUSTAINING
//   voice - it decays to 0.7 and holds, and its pitch continuously follows
//   the quantized PITCH target. A new fire demotes it (decays to zero) and
//   takes over. Entering FLOW with no sustaining voice auto-triggers one at
//   the current PITCH target (the drone promise) - deferred to the next
//   process() call so the targets are fresh.
// - Targets: TIMBRE (morph + t^2 * DETUNE_MAX detune), FILTER (60 Hz-14 kHz
//   exp), PITCH (latched at trigger, 110*8^p), MOTION (pan fan
//   [-1,+1,-0.5,+0.5] * width + drift ~ width), LEVEL (OnePole-smoothed
//   master gain). All but PITCH act on all voices continuously.
// - Control rate: drift LFOs + envelope coefficients + all voice parameter
//   pushes run once per kCtrlInterval samples (CPU-budget constraint).
class SynthEngine : public IPartEngine {
public:
    static constexpr int   kVoices       = 4;
    static constexpr int   kCtrlInterval = 96;
    static constexpr float kAttackFloorS = 0.002f;
    static constexpr float kDecayMinS    = 0.05f;
    static constexpr float kDecayMaxS    = 20.f;
    static constexpr float kDetuneCeilCt = 35.f;

    void set_seed(uint32_t seed) { _seed = seed; }   // call BEFORE init

    void init(float sample_rate) override;
    void set_targets(const float* t, float tune) override;
    void trigger(float pitch_norm) override;
    void process(float& outL, float& outR) override;
    void set_cycle(float seconds) override;
    void set_flow(bool flow) override;
    void set_hold(bool on) override;

    // VOICE edit layer (normalized knobs; boot defaults live as raw ratios)
    void set_attack(float n);      // ratio = 0.002 * 250^n  (0.2%..50% of cycle)
    void set_decay(float n);       // ratio = 0.1 * 80^n     (0.1x..8x cycle)
    void set_resonance(float n);
    void set_sub(float n);
    void set_detune(float n);      // DETUNE_MAX = n * 35 ct

    int   active_voices() const;
    float voice_env(int v) const;
    int   sustain_voice() const { return _sustain_voice; }

private:
    void _do_trigger(float pitch_norm);
    void _update_control();

    std::array<Voice, kVoices>    _voices;
    std::array<uint32_t, kVoices> _order {};   // trigger sequence per voice
    uint32_t _seq = 0;
    uint32_t _seed = 0xC0FFEEu;

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };
    bool  _flow = false;
    bool  _hold = false;   // CHOKE: drone released + auto-retrigger paused
    int   _sustain_voice = -1;     // -1 = none
    bool  _auto_pending = false;   // drone promise, fires in process()
    int   _next_rr = 0;
    int   _ctrl_ctr = 0;

    float _cycle_s = 1.f;
    float _attack_ratio = 0.02f;   // boot: 2 % of cycle (spec)
    float _decay_ratio  = 1.5f;    // boot: 1.5 x cycle (spec)
    float _resonance = 0.15f;      // boot (spec)
    float _sub_level = 0.3f;       // boot (spec)
    float _detune_max_ct = 18.f;   // boot DETUNE_MAX (spec)

    OnePole _level;                // smoothed master gain (LEVEL target)
};

} // namespace spky
