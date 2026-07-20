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
    // by this, so ~4 grains sound at once and 8 slots leave scatter headroom.
    static constexpr int kOverlap      = 4;

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

    float _sr = 48000.f;
    float _targets[LANE_COUNT] = { 0.f, 0.5f, 0.5f, 0.f, 0.8f };

    // derived at the control tick
    float _grain_len   = 960.f;   // output samples
    float _spawn_every = 240.f;   // samples between spawns
    float _filt_gain   = 1.f;

    float _spawn_ctr   = 0.f;
    int   _ctrl_ctr    = 0;
    int   _rr          = 0;       // chord round-robin cursor
    int   _release_ctr = 0;       // STEP burst release, in samples

    float _chord[kMaxChord] = { 0.5f, 0.5f, 0.5f, 0.5f };
    int   _chord_n = 1;
    float _burst_pitch   = 0.5f;
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
