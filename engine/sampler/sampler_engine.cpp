#include "sampler/sampler_engine.h"
#include "util/math.h"
#include <cmath>

namespace spky {

namespace {
using namespace sampler_cfg;

// Pitch: the lane arrives already quantized from Part. The synth's mapping is
// 110*8^p Hz, so p = 0.5 is unity here and the range is +-18 semitones.
inline float ratio_for(float pitch_norm) {
    return std::pow(8.f, clampf(pitch_norm, 0.f, 1.f) - 0.5f);
}
inline float size_seconds(float n) {
    return kSizeMinS * std::pow(kSizeRange, clampf(n, 0.f, 1.f));
}
inline float cutoff_hz(float n) {
    return kCutoffMinHz * std::pow(kCutoffMaxHz / kCutoffMinHz, clampf(n, 0.f, 1.f));
}
}  // namespace

void SamplerEngine::init(float sample_rate) {
    _sr = sample_rate;
    _buf.init(_mem, _mem_frames, sample_rate);
    _rng.seed(_seed ^ 0x5A11E20Du);

    _svf_l.Init(sample_rate);
    _svf_r.Init(sample_rate);
    _svf_l.SetFreq(kCutoffMaxHz);
    _svf_r.SetFreq(kCutoffMaxHz);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
    _svf_l.SetDrive(0.f);
    _svf_r.SetDrive(0.f);

    _level.init(sample_rate, 0.01f);   // 10 ms, as the synth's LEVEL
    _kill_all();
    _spawn_ctr   = 0.f;
    _ctrl_ctr    = 0;
    _release_ctr = 0;
    _update_control();
}

void SamplerEngine::set_targets(const float* t, float tune) {
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
    _tune = tune;
}

void SamplerEngine::set_flow(bool flow) {
    if (flow == _flow) return;
    _flow = flow;
    if (!_flow) _release_ctr = 0;      // leaving FLOW: running grains decay out
}

void SamplerEngine::set_hold(bool on) {
    if (on == _hold) return;
    _hold = on;
    if (_hold) _release_all();
}

void SamplerEngine::set_gate(bool on) {
    if (on == _gate) return;
    _gate = on;
    if (!on) {
        _release_ctr = static_cast<int>(kBurstReleaseS * _sr);
    } else {
        // Start the burst on the edge, not up to _spawn_every samples late:
        // leaving FLOW mid-cycle (or a prior STEP burst) can leave _spawn_ctr
        // anywhere in [0, _spawn_every), and STEP is supposed to reproduce
        // the phrase generator's composed rhythm exactly.
        _spawn_ctr = 0.f;
    }
}

void SamplerEngine::process_in(float inL, float inR) {
    _in_l = inL;
    _in_r = inR;
    _buf.write(inL, inR);              // no-op unless recording
}

void SamplerEngine::set_recording(bool on) { _buf.set_recording(on); }

void SamplerEngine::load_sample(const float* l, const float* r, size_t frames) {
    if (!_buf.valid() || l == nullptr) return;
    _kill_all();
    _buf.clear();
    const size_t n = frames > _buf.capacity() ? _buf.capacity() : frames;
    SampleBuffer::Frame* dst = _buf.raw();
    for (size_t i = 0; i < n; ++i) {
        dst[i].l = l[i];
        dst[i].r = r ? r[i] : l[i];    // mono normals to both channels
    }
    _buf.set_rec_size(n);
}

void SamplerEngine::trigger(float pitch_norm) {
    _burst_pitch   = pitch_norm;
    _burst_latched = true;
    _chord[0] = pitch_norm;
    _chord_n  = 1;
    _rr = 0;
}

void SamplerEngine::trigger_chord(const float* p, int n) {
    if (n < 1) return;
    if (n > kMaxChord) n = kMaxChord;
    for (int i = 0; i < n; ++i) _chord[i] = p[i];
    _chord_n       = n;
    _burst_pitch   = p[0];
    _burst_latched = true;
    _rr = 0;
}

void SamplerEngine::set_chord(const float* p, int n) {
    if (n < 1) return;
    if (n > kMaxChord) n = kMaxChord;
    for (int i = 0; i < n; ++i) _chord[i] = p[i];
    _chord_n = n;
}

int SamplerEngine::active_grains() const {
    int n = 0;
    for (int i = 0; i < kGrains; ++i) if (_grains[i].active()) ++n;
    return n;
}

void SamplerEngine::_kill_all() {
    for (int i = 0; i < kGrains; ++i) _grains[i].kill();
    // The scheduler must restart with the grains. Every caller of _kill_all
    // (init, clear, load_sample) leaves no running grain behind to mask a
    // pending countdown, so a stale _spawn_ctr would hold the cloud silent
    // for up to one spawn interval -- half a second at long SIZE, on the
    // ordinary load path. Unlike the SIZE-drop case, nothing masks this.
    _spawn_ctr = 0.f;
}

void SamplerEngine::_release_all() {
    const int fade = static_cast<int>(kRecordFade);
    for (int i = 0; i < kGrains; ++i)
        if (_grains[i].active()) _grains[i].release(fade);
}

// Task 4 replaces this body with the chord round-robin and octave scatter.
// Until then: the latched burst pitch in STEP, the live lane in FLOW.
float SamplerEngine::_next_ratio() {
    const float p = (!_flow && _burst_latched) ? _burst_pitch : _targets[LANE_PITCH];
    return ratio_for(p);
}

void SamplerEngine::_update_control() {
    // --- SIZE: exponential 20 ms .. 2 s, clamped to what we actually have ---
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    const float content = static_cast<float>(_buf.rec_size());
    if (content > 1.f && len > content) len = content;
    // Floored well above the degenerate case (first samples of a punch-in,
    // or a <=4-frame file): a tiny _grain_len drives _spawn_every to its
    // 1-sample floor, which would run _spawn_one -> _next_ratio -> the
    // std::pow in ratio_for() every sample. Sub-64-sample grains are
    // musically meaningless anyway, so keep spawn-time std::pow off the
    // per-sample path by never asking for grains that short.
    if (len < 64.f) len = 64.f;
    _grain_len = len;

    _spawn_every = _grain_len / static_cast<float>(kOverlap);
    if (_spawn_every < 1.f) _spawn_every = 1.f;

    // A shrinking interval must not leave a stale long countdown pending:
    // sweeping SIZE down would otherwise gap the carpet for up to the old
    // interval while grains retire at the new, much shorter length.
    if (_spawn_ctr > _spawn_every) _spawn_ctr = _spawn_every;

    // --- overlap normalization: 1/sqrt(active), the COLOR loudness law ---
    const int n = active_grains();
    _norm = n > 0 ? 1.f / std::sqrt(static_cast<float>(n)) : 1.f;

    // --- FILT: same bipolar rails as the synth (Task 5 gives it a knob) ---
    const float off   = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
    const float n_raw = _targets[LANE_SIZE] + off;
    _filt_gain = clampf(1.f + n_raw / kFiltFadeRange, 0.f, 1.f);
    const float hz = cutoff_hz(n_raw);
    _svf_l.SetFreq(clampf(hz, 20.f, 0.3f * _sr));
    _svf_r.SetFreq(clampf(hz, 20.f, 0.3f * _sr));
}

void SamplerEngine::_spawn_one() {
    if (_buf.is_empty()) return;

    int slot = -1;
    for (int i = 0; i < kGrains; ++i)
        if (!_grains[i].active()) { slot = i; break; }
    if (slot < 0) return;                       // all busy: skip this spawn

    // Fill-follows: SOURCE maps into the CURRENT content length, so while a
    // recording runs the cloud granulates only what is already captured.
    const float content = static_cast<float>(_buf.rec_size());
    const float centre  = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * content;

    const float ratio = _next_ratio();
    const int   len   = static_cast<int>(_grain_len);
    const int   half  = static_cast<int>(_grain_len * kWindowHalfMin);
    const int   atk   = half < 1 ? 1 : half;

    _grains[slot].spawn(centre, ratio, 0.f, len, atk, atk, _reverse);
    _last_pos = centre;
}

void SamplerEngine::process(float& outL, float& outR) {
    if (_ctrl_ctr == 0) {
        _ctrl_ctr = kCtrlInterval;
        _update_control();
    }
    --_ctrl_ctr;

    // --- scheduling ---
    const bool spawning = !_hold && (_flow || _gate || _release_ctr > 0);
    if (_release_ctr > 0 && !_gate) --_release_ctr;

    if (spawning) {
        _spawn_ctr -= 1.f;
        if (_spawn_ctr <= 0.f) {
            _spawn_one();
            _spawn_ctr += _spawn_every;
            if (_spawn_ctr < 1.f) _spawn_ctr = 1.f;
        }
    }

    // --- the cloud ---
    float l = 0.f, r = 0.f;
    for (int i = 0; i < kGrains; ++i) {
        if (!_grains[i].active()) continue;
        float gl = 0.f, gr = 0.f;
        _grains[i].process(_buf, gl, gr);
        l += gl;
        r += gr;
    }
    l *= _norm;
    r *= _norm;

    // --- filter, then LEVEL with the FILT silence fade folded in ---
    _svf_l.Process(l);
    _svf_r.Process(r);
    l = _svf_l.Low();
    r = _svf_r.Low();

    const float gain = _level.process(clampf(_targets[LANE_LEVEL], 0.f, 1.f) * _filt_gain);
    l *= gain;
    r *= gain;

    // --- monitoring: dry input at unity, ahead of the part chain (pre-GRIT) ---
    if (_monitor) { l += _in_l; r += _in_r; }

    outL = l;
    outR = r;
}

// --- voice row: behaviour lands in Task 5; the state is stored here ---
void SamplerEngine::set_window_attack(float n) { _atk_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_window_decay(float n)  { _dec_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_filt(float n)          { _filt_amt = clampf(n, -1.f, 1.f); }
void SamplerEngine::set_resonance(float n) {
    _res_n = clampf(n, 0.f, 0.95f);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
}
void SamplerEngine::set_sub(float n)    { _sub_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_detune(float n) { _detune_n = clampf(n, 0.f, 1.f); }

}  // namespace spky
