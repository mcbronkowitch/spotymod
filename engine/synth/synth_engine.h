#pragma once
#include <array>
#include <cstdint>
#include "mod/rng.h"
#include "parts/engine_iface.h"
#include "pitch/chord.h"
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
    static constexpr int   kMaxChord     = 4;
    static constexpr float kStabSpreadS  = 0.008f;   // stab humanization (ear-tunable)

    static_assert(kMaxChord == ChordBuilder::kMaxNotes,
                  "chord slot count must match the builder");

    // FILT: linke Haelfte uebersteuert die Schiene um genau die Blendzone,
    // damit t = -1 bei JEDER Lane-Stellung in Stille endet (Invariante:
    // kFiltLeftScale >= 1 + kFiltFadeRange).
    static constexpr float kFiltLeftScale = 1.25f;
    static constexpr float kFiltFadeRange = 0.25f;

    void set_seed(uint32_t seed) { _seed = seed; }   // call BEFORE init

    void init(float sample_rate) override;
    void set_targets(const float* t, float tune) override;
    void trigger(float pitch_norm) override;
    void trigger_chord(const float* pitches_norm, int n) override;
    void set_chord(const float* pitches_norm, int n) override;
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
    void set_filt(float n);        // -1..+1 cutoff trim; left end fades to silence

    int   active_voices() const;
    float voice_env(int v) const;
    int   sustain_voice() const;
    int   sustain_count() const;

    // --- observation (tests) ---
    // Raw values behind set_sub()/set_detune(), exposed so a test can pin the
    // Synth-only leg of the SUB/DTUN split (spec 2026-07-21
    // morphagene-controls, Part::set_voice_sub/set_voice_detune) without
    // reaching into private state. Not used on the audio path.
    float sub_level() const     { return _sub_level; }
    float detune_max_ct() const { return _detune_max_ct; }

private:
    void _do_trigger(float pitch_norm, float vel, int chord_slot);
    void _demote_all();
    void _update_control();
    void _adjust_surface();

    std::array<Voice, kVoices>    _voices;
    std::array<uint32_t, kVoices> _order {};   // trigger sequence per voice
    uint32_t _seq = 0;
    uint32_t _seed = 0xC0FFEEu;

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };
    bool  _flow = false;
    bool  _hold = false;   // CHOKE: drone released + auto-retrigger paused
    bool  _auto_pending = false;   // drone promise, fires in process()
    int   _next_rr = 0;
    int   _ctrl_ctr = 0;

    float _chord[kMaxChord] = { 0.f, 0.f, 0.f, 0.f };   // surface targets (Part)
    int   _chord_n = 1;
    bool  _sustaining[kVoices] = { false, false, false, false };
    int   _chord_slot[kVoices] = { -1, -1, -1, -1 };
    struct Pending { int ctr; float pitch; int slot; };
    Pending _pending[kMaxChord] = {};
    int   _pending_n = 0;
    float _vel_now = 1.f;
    Rng   _stab_rng;                                    // draws ONLY for n>=2 chords

    float _cycle_s = 1.f;
    float _attack_ratio = 0.02f;   // boot: 2 % of cycle (spec)
    float _decay_ratio  = 1.5f;    // boot: 1.5 x cycle (spec)
    float _resonance = 0.15f;      // boot (spec)
    float _sub_level = 0.3f;       // boot (spec)
    float _detune_max_ct = 18.f;   // boot DETUNE_MAX (spec)
    float _filt_amt  = 0.f;        // FILT knob -1..+1 (boot: neutral)
    float _filt_gain = 1.f;        // silence fade below the 60 Hz rail (control-rate)

    OnePole _level;                // smoothed master gain (LEVEL target)
};

} // namespace spky
