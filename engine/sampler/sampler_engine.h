#pragma once
#include <cstdint>
#include "mod/rng.h"
#include "parts/engine_iface.h"
#include "sampler/grain.h"
#include "sampler/sample_buffer.h"
#include "sampler/sampler_config.h"
#include "util/onepole.h"
#include "Filters/svf.h"      // daisysp::Svf -- the engine's only DaisySP dep

namespace spky {

// Test seam only: forwards to the anonymous-namespace helper in
// sampler_engine.cpp so the approximation can be checked against std::pow
// without exposing the whole translation unit.
float test_detune_factor(float cents);

// Test seam only: forwards to the anonymous-namespace SIZE mapping in
// sampler_engine.cpp, so the curve can be checked point by point without
// driving a whole engine.
float test_size_seconds(float n);

// Test seam only: forwards to the anonymous-namespace spawn-interval helper
// in sampler_engine.cpp, so the CPU floor can be checked at overlaps that
// engage it (today's kOverlap = 4 does not) without driving a whole engine.
float test_spawn_interval(float grain_len, int overlap);

// Test seam only: forwards to the anonymous-namespace pitch mapping.
float test_ratio_for(float pitch_norm);

// The M5 texture deck: a granular cloud behind IPartEngine.
//
// Not a second melodic instrument -- the synth part makes the music, this
// makes the room. Material comes from live input (process_in + set_recording)
// or from a loaded WAV (load_sample); the cloud granulates whatever is
// already captured, including while a recording is still running.
//
// - FLOW: a standing cloud. Grains respawn continuously; the lanes shape the
//   texture and it never gaps.
// - STEP: groove-gated bursts. Grains spawn only while the gate is high plus
//   a short release, so the phrase generator's composed rhythm, DENSITY,
//   CHOKE windows and the GATE jack all chop the texture for free.
class SamplerEngine : public IPartEngine {
public:
    static constexpr int kGrains       = sampler_cfg::kGrains;
    static constexpr int kCtrlInterval = sampler_cfg::kCtrlInterval;
    static constexpr int kMaxChord     = 4;
    // Target overlap in FLOW: the spawn interval is one grain length divided
    // by this. This is the density control, NOT kGrains -- kGrains only
    // supplies slots, and a spawn that finds none free is silently dropped
    // (see SamplerEngine::_spawn_one) -- so the two must move together or
    // raising one alone does nothing audible.
    //
    // Chosen from measurement: the worst-case telemetry case ("sampler:
    // density telemetry at worst case" -- MOTION=1, SIZE=0.1, FLOW, 10 s)
    // paired with the 40 s sampler_storm.json render (desktop wall-clock, a
    // PROXY for Daisy cost, not the cost itself -- see below):
    //   (4, 8):   mean 4.21 active, peak 8/8    (pinned at kGrains -- already
    //             dropping spawns under worst case, at today's baseline)
    //             render 2.52 s / 40 s audio = 0.063 s per audio-s
    //   (8, 16):  mean 8.13 active, peak 13/16
    //             render 2.63 s / 40 s audio = 0.066 s per audio-s (+4%)
    //   (16, 32): mean 16.09 active, peak 24/32
    //             render 2.92 s / 40 s audio = 0.073 s per audio-s (+16%)
    // The kSpawnMinSamples floor ("the spawn interval never falls below its
    // floor") passes at every pair -- dormant at (4, 8), and exactly at its
    // bound (6001 <= 6001) from (8, 16) upward, confirming it is now the
    // thing actually capping the spawn rate.
    //
    // Chosen: (8, 16). The desktop proxy shows only a mild compute cost even
    // at (16, 32), but it is blindest exactly there: the Daisy's real limit
    // at that density is SDRAM traffic from interpolated reads scattered
    // across a ~32 MB record buffer, defeating the cache in a way a desktop
    // CPU with megabytes of cache does not feel at all. (8, 16) roughly
    // doubles the cloud over today's baseline -- which was itself already
    // silently dropping spawns under worst-case MOTION -- while staying
    // clear of the density this measurement cannot actually vouch for.
    static constexpr int kOverlap      = 8;

    // Both must be called BEFORE init(), matching SynthEngine::set_seed.
    void set_seed(uint32_t s) { _seed = s; }
    void set_memory(SampleBuffer::Frame* buf, size_t frames) {
        _mem = buf; _mem_frames = buf ? frames : 0;
    }

    // --- IPartEngine ---
    void init(float sample_rate) override;
    void set_targets(const float* t, float tune) override;
    void trigger(float pitch_norm) override;
    void trigger_chord(const float* pitches_norm, int n) override;
    void set_chord(const float* pitches_norm, int n) override;
    void process(float& outL, float& outR) override;
    void set_flow(bool flow) override;
    void set_hold(bool on) override;
    void set_gate(bool on) override;
    void process_in(float inL, float inR) override;

    // --- material ---
    void   set_recording(bool on);
    bool   is_recording() const { return _buf.is_recording(); }
    float  buffer_fill() const  { return _buf.fill(); }
    bool   is_empty() const     { return _buf.is_empty(); }
    size_t rec_size() const     { return _buf.rec_size(); }
    void   clear()              { _kill_all(); _buf.clear(); }
    void   set_monitor(bool on) { _monitor = on; }
    void   load_sample(const float* l, const float* r, size_t frames);

    // --- edit layer ---
    void set_tape_mode(bool tape) { _tape = tape; }
    void set_reverse(bool on)     { _reverse = on; }
    void set_feedback(float knob) { _buf.set_feedback(knob); }

    // --- voice row, remapped ---
    void set_window_attack(float n);
    void set_window_decay(float n);
    void set_filt(float n);
    void set_resonance(float n);
    void set_sub(float n);
    void set_detune(float n);

    // --- observation (CSV, tests) ---
    int   active_grains() const;
    float grain_len_samples() const { return _grain_len; }
    int   spawn_count() const       { return _spawn_count; }
    float last_spawn_ratio() const  { return _last_ratio; }
    float last_spawn_pan() const    { return _last_pan; }
    float last_spawn_pos() const    { return _last_pos; }
    int   last_spawn_len() const    { return _last_len; }

private:
    void  _update_control();     // recompute derived values on the raster
    void  _spawn_one();          // spawn into a free slot, if any
    void  _kill_all();
    void  _release_all();
    float _next_ratio();         // chord round-robin + octave scatter

    SampleBuffer _buf;
    SampleBuffer::Frame* _mem = nullptr;
    size_t _mem_frames = 0;

    Grain _grains[kGrains];
    Rng   _rng;
    uint32_t _seed = 0xC0FFEEu;

    daisysp::Svf _svf_l, _svf_r;
    OnePole _level;
    OnePole _norm;          // smoothed 1/sqrt(active) -- see _update_control

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };

    // derived at the control tick
    float _grain_len   = 960.f;   // output samples
    float _spawn_every = 240.f;   // samples between spawns
    float _filt_gain   = 1.f;
    float _norm_target = 1.f;     // 1/sqrt(active), fed through _norm per sample

    float _spawn_ctr   = 0.f;
    int   _ctrl_ctr    = 0;
    int   _rr          = 0;       // chord round-robin cursor
    int   _release_ctr = 0;       // STEP burst release, in samples

    float _chord[kMaxChord] = { 0.5f, 0.5f, 0.5f, 0.5f };
    // ratio_for(_chord[i]) for i < _chord_n, refreshed once per control tick.
    // _next_ratio runs on the grain-spawn path, and std::pow must not.
    float _chord_ratio[kMaxChord] = {};
    int   _chord_n = 1;
    float _burst_pitch   = 0.5f;
    // ratio_for(_burst_pitch), cached at the trigger that sets _burst_pitch.
    // This is a SEPARATE cache from _chord_ratio[] on purpose: _chord_ratio[]
    // tracks _chord[] live (refreshed every control tick, per _update_control),
    // but _next_ratio's latched single-note branch must NOT track live pitch
    // -- that is exactly what "latched" means for that branch (see the
    // comment in _next_ratio). Reading _chord_ratio[0] there instead would
    // replace the frozen trigger-time pitch with whatever _chord[0] has
    // drifted to since (e.g. PITCH vibrato under a held STEP gate), which is
    // a real behaviour change, not a refactor. Default 1.0 == ratio_for(0.5f),
    // matching what _burst_pitch's own 0.5f default would produce, so a spawn
    // that somehow precedes any trigger reads unity, not silence or NaN.
    float _burst_ratio   = 1.f;
    // Set by trigger/trigger_chord, persists until the next trigger -- that
    // is what "trigger latches the pitch for the burst" means. Part::process
    // calls trigger_chord BEFORE forwarding the gate, so set_gate must not
    // clear this or the latch would be wiped every time.
    bool  _burst_latched = false;
    float _last_pos = 0.f;
    int   _spawn_count = 0;
    float _last_ratio  = 1.f;
    float _last_pan    = 0.f;
    int   _last_len    = 0;
    float _spawn_jitter = 0.f;   // spawn-interval jitter applied to the NEXT interval

    float _in_l = 0.f, _in_r = 0.f;

    bool _flow    = false;
    bool _hold    = false;
    bool _gate    = false;
    bool _monitor = false;
    bool _tape    = false;
    bool _reverse = false;

    // voice row
    float _atk_n = 0.3f, _dec_n = 0.3f;
    float _filt_amt = 0.f, _res_n = 0.15f, _sub_n = 0.f, _detune_n = 0.f;
};

}  // namespace spky
