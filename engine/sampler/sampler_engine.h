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
// in sampler_engine.cpp, so the CPU floor can be checked at chosen overlaps
// without driving a whole engine. It was extracted when kOverlap was 4 and
// the floor never fired along any reachable path; at today's kOverlap = 8
// the floor DOES fire (SIZE 0 gives a 48-sample grain, 48 / 8 = 6, lifted
// to kSpawnMinSamples = 8), so this seam now pins a live guard as well as
// covering overlaps the engine does not currently use.
float test_spawn_interval(float grain_len, int overlap);

// Test seam only: forwards to the anonymous-namespace pitch mapping.
float test_ratio_for(float pitch_norm);

// Test seam only: forwards to the anonymous-namespace SCAN curve helper in
// sampler_engine.cpp, so the dead zone, both knees and the endpoints can be
// pinned without driving a whole engine.
float test_scan_rate(float n);

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
    //   (4, 8):   mean 4.21 active, peak 8/8    (pinned at kGrains -- but a
    //             pinned peak only means the pool was momentarily full, not
    //             that a spawn was lost; counting actual drops directly
    //             (SamplerEngine::dropped_spawns(), added after this pair
    //             was retired) puts this same worst case at 1 dropped of
    //             5962 attempted spawns, 0.017% -- negligible, not the
    //             "already dropping spawns" reading the pinned peak alone
    //             suggested)
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
    // doubles the cloud over the old (4, 8) baseline -- which, measured
    // directly, was NOT silently dropping spawns under worst-case MOTION in
    // any meaningful sense (0.017% of attempts, see above) -- while staying
    // clear of the density this measurement cannot actually vouch for. The
    // move to (8, 16) stands on the desktop/SDRAM cost argument above, not
    // on the old pair having been a problem.
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

    // Read the recorded content back out: Save sample..., the patch-storage
    // autosave, and the host's sample-rate snapshot all need it. This hands
    // back the very pointer the host injected, so the caller must respect
    // rec_size() as the valid length -- past it lie stale frames. nullptr
    // when no memory was injected.
    //
    // Note for callers carrying content across an init(): copy OUT first.
    // init() ends in clear(), but clear() only memsets the buffer when it
    // held content going in (_size != 0) -- see SampleBuffer::clear()'s I-3
    // fast path. A buffer that never held content (_size == 0, e.g. memory
    // a host just injected and has not written to yet) is NOT zeroed by
    // init()/clear(). Hosts must inject already-zeroed memory: this matters
    // for any host that carves buffers out of memory the platform does not
    // zero at startup (e.g. Daisy SDRAM), where the overdub read-before-
    // write path would otherwise read uninitialised garbage.
    const SampleBuffer::Frame* sample_data() const { return _buf.raw(); }

    // --- edit layer ---
    void set_tape_mode(bool tape) { _tape = tape; }
    void set_reverse(bool on)     { _reverse = on; }
    void set_feedback(float knob) { _buf.set_feedback(knob); }

    // DENS in the sampler: grain overlap, 1..8 (spec 2026-07-21
    // morphagene-controls). n is a knob position 0..1. This is the density
    // control that kOverlap used to fix at compile time; at low overlap the
    // grain window stops being a Constant-OverLap-Add system and ATK/DEC
    // become audible.
    void set_overlap(float n);

    // SCAN: the running playhead (spec 2026-07-21 morphagene-controls).
    // bipolar is -1..+1; the sign is the direction, the centre is a real dead
    // zone. The accumulated position is ADDED to the SOURCE target in
    // _spawn_one, so ORGANIZE sets where the head starts and SCAN moves it.
    //
    // Calls std::pow (scan_rate(), sampler_engine.cpp) -- a control-rate-only
    // caller, never the per-sample audio path. This engine's own control tick
    // is kCtrlInterval = 96 samples (~2 ms @ 48 kHz); the VCV host currently
    // calls this more often than that -- ctrlDiv divides by 16 samples
    // (host/vcv/src/Spotymod.cpp), i.e. every ~0.33 ms @ 48 kHz. Measured
    // affordable at that rate; a future caller pushing this every audio
    // sample would put a std::pow on the per-sample path and must not.
    void set_scan(float bipolar);

    // "New gene now" (spec 2026-07-21 morphagene-controls): the playhead
    // returns to ORGANIZE and a grain spawns immediately. Wired to NEW and
    // TRIG in the sampler.
    //
    // Deliberately NOT routed through _kill_all: this is a gesture on a
    // running cloud, so grains that are already sounding keep sounding. It is
    // what makes the long-grain end of SIZE playable at all -- without it,
    // every knob is dead until the next scheduled spawn, which at overlap 1
    // and SIZE near the top is tens of seconds away.
    void punch();

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
    float overlap() const               { return _overlap; }
    float spawn_interval_samples() const { return _spawn_every; }
    // Accumulated playhead offset in frames, folded into [0, rec_size).
    // Drives the VCV ring's read-position dot as well as the tests.
    float scan_pos() const { return _scan_pos; }
    int   spawn_count() const       { return _spawn_count; }
    // Incremented in _spawn_one when every slot is busy and the spawn is
    // skipped -- the exact moment a spawn is lost. Never reset except by
    // construction (matches _spawn_count, which has no reset path either).
    int   dropped_spawns() const    { return _dropped_spawns; }
    float last_spawn_ratio() const  { return _last_ratio; }
    float last_spawn_pan() const    { return _last_pan; }
    // SUB and DTUN no longer reach the sampler (spec 2026-07-21
    // morphagene-controls); these stay at their silent 0.f defaults. Exposed
    // so tests can pin the disconnection down.
    float sub() const    { return _sub_n; }
    float detune() const { return _detune_n; }
    float last_spawn_pos() const    { return _last_pos; }
    int   last_spawn_len() const    { return _last_len; }

private:
    void  _update_control();     // recompute derived values on the raster
    // Das effektive Intervall bis zum naechsten Spawn: Grundintervall mal
    // Timing-Jitter, danach auf kSpawnMinSamples gebodet. Zwei Aufrufer, und
    // dass es DIESELBE Zahl ist, ist der ganze Punkt -- process() zaehlt
    // damit herunter, _update_control clamped dagegen. Rechnete der Clamp mit
    // dem ungejitterten _spawn_every, kappte er jedes zu LANGE Intervall
    // weg und liess jedes zu kurze stehen, womit ein symmetrisch gezogener
    // Jitter die Wolke systematisch beschleunigte (bis +21 % bei MOTION 1).
    // Der Boden gehoert hierher und nicht in spawn_interval(): dort steht er
    // vor dem Jitter, und der Jitter unterlief ihn anschliessend bis auf 2
    // Samples -- das Vierfache der in sampler_config.h zugesagten 6 kHz.
    float _next_interval() const;
    void  _spawn_one();          // spawn into a free slot, if any
    void  _trim_running();       // SIZE turned down: shorten what is sounding
    void  _kill_all();
    void  _release_all();
    float _next_ratio();         // chord round-robin + octave scatter

    SampleBuffer _buf;
    SampleBuffer::Frame* _mem = nullptr;
    size_t _mem_frames = 0;

    Grain _grains[kGrains];
    // _grain_len at the instant slot i was spawned, and the length it was
    // given. _trim_running needs both to rescale a running grain to "what it
    // would have been at today's SIZE" without asking Grain to know anything
    // about SIZE, tape mode or ratios -- the policy stays here, where SIZE
    // lives, and Grain stays a playback object. Zero means "no live grain".
    float _size_ref[kGrains]  = {};
    float _len_ref[kGrains]   = {};
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
    float _overlap     = static_cast<float>(kOverlap);   // 1..8, DENS
    float _scan_rate   = 0.f;     // frames per sample, signed
    float _scan_pos    = 0.f;     // accumulated offset in frames, folded
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
    int   _dropped_spawns = 0;
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
