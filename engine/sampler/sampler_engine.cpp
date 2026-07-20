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
    // MUST differ from the constant Part::init XORs in (0x5A11E20Du). With
    // the same constant on both sides the XOR cancels and the sampler is
    // seeded with seed_base exactly -- which is also what SuperModulator
    // gives lane 0 (seed_base + 0). Both are xorshift32, so MOTION scatter
    // would run on a bit-identical stream to the PITCH lane. The synth
    // avoids this by using two different constants (part.cpp:17,
    // synth_engine.cpp:39); follow that.
    _rng.seed(_seed ^ 0xC0FFEE11u);

    _svf_l.Init(sample_rate);
    _svf_r.Init(sample_rate);
    _svf_l.SetFreq(kCutoffMaxHz);
    _svf_r.SetFreq(kCutoffMaxHz);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
    _svf_l.SetDrive(0.f);
    _svf_r.SetDrive(0.f);

    _level.init(sample_rate, 0.01f);   // 10 ms, as the synth's LEVEL
    _norm.init(sample_rate, kNormSmoothS);
    _norm.reset(1.f);   // no grains yet == the n==0 baseline _update_control gives
    _kill_all();
    _spawn_ctr   = 0.f;
    _ctrl_ctr    = 0;
    _release_ctr = 0;
    _update_control();
}

void SamplerEngine::set_targets(const float* t, float /*tune*/) {
    for (int i = 0; i < LANE_COUNT; ++i) _targets[i] = t[i];
    // tune is already summed into the quantized PITCH target upstream
    // (Part::pitch_pre_quant), same as SynthEngine::set_targets
    // (synth_engine.cpp:67-68) -- nothing left for this engine to do with it.
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
        // DEC stretches the burst tail: a long decay leaves a longer trail
        // after the composed note ends (spec, voice-row table).
        const float rel = kBurstReleaseS * (0.5f + 1.5f * _dec_n);
        _release_ctr = static_cast<int>(rel * _sr);
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

// Round-robin over the current chord. COLOR 0 leaves _chord_n == 1, so every
// grain lands on the root and the single-note world is untouched -- the
// chord layer's bit-identity promise, carried into the cloud.
//
// MOTION adds a mild octave scatter on top: at full scatter, kScatterOctProb
// of grains jump an octave up or down. The roll draw (next_unipolar() below)
// happens for every spawn regardless of MOTION, so THAT draw does not change
// the Rng stream's shape with the knob. The octave-DIRECTION draw right
// after it does not share that property: it only runs when the roll
// succeeds, so how far into the stream the following draws (timing, sub,
// detune) land shifts with MOTION and with luck on the roll. This is known
// and accepted -- it is not the same "every spawn draws the same shape"
// guarantee the roll itself has.
float SamplerEngine::_next_ratio() {
    const bool  latched = (!_flow && _burst_latched);
    const float motion  = clampf(_targets[LANE_MOTION], 0.f, 1.f);

    float p;
    if (latched && _chord_n <= 1) {
        p = _burst_pitch;
    } else {
        // COLOR > 0 (_chord_n > 1) takes this branch even during a latched
        // STEP burst, and reads _chord[] live rather than a value frozen at
        // trigger time. Part::_control_tick refreshes _chord[] every 96
        // samples, so the note set a round-robin burst is drawing from can
        // change mid-burst if the composed chord changes underneath it. The
        // spec (sampler-texture-deck-design.md) says trigger "latches the
        // pitch (or chord set) for the burst" -- for chords (COLOR > 0) the
        // current code does not do that; only the single-note path above
        // (_chord_n <= 1) is actually latched. This is a known deviation,
        // not a bug to silently fix: whether a burst should freeze its whole
        // chord set at the gate edge is a musical call for the instrument's
        // author, not an engine-layer decision.
        p = _chord[_rr % _chord_n];
        _rr = (_rr + 1) % _chord_n;
    }

    float ratio = ratio_for(p);

    const float roll = _rng.next_unipolar();
    if (motion * kScatterOctProb > roll)
        ratio *= _rng.next_unipolar() < 0.5f ? 0.5f : 2.f;

    return ratio;
}

void SamplerEngine::_update_control() {
    // --- SIZE: exponential 20 ms .. 2 s, clamped to what we actually have ---
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    // MOTION stretches grain length on top of SIZE (spec amendment 2026-07-20
    // (2)): applied before the content clamp, so a long SIZE at full MOTION
    // still bottoms out at the available content, and _spawn_every (derived
    // from _grain_len below) scales together with it -- keeping the overlap
    // ratio at kOverlap rather than climbing toward the kGrains ceiling.
    const float motion = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    len *= 1.f + motion * kScatterSmear;
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

    // Overlap normalization: 1/sqrt(active), the COLOR loudness law. Computed
    // globally here, once per control tick, from the currently-sounding
    // grain count -- NOT latched per grain (see grain.h for why that
    // analogy to SynthEngine::trigger_chord fails: grains overlap
    // independently, with no group whose count stays fixed for a latched
    // grain's whole life). Recomputing this every tick and applying it raw
    // to the summed cloud steps the output audibly on every spawn/retire, so
    // process() feeds the target through _norm (a OnePole) instead of
    // assigning it directly.
    const int n = active_grains();
    _norm_target = n > 0 ? 1.f / std::sqrt(static_cast<float>(n)) : 1.f;

    // --- FILT: same bipolar rails as the synth; set_filt() is the knob ---
    const float off   = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
    const float n_raw = kFiltNeutral + off;
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

    const float motion  = clampf(_targets[LANE_MOTION], 0.f, 1.f);
    // Fill-follows: SOURCE maps into the CURRENT content length, so while a
    // recording runs the cloud granulates only what is already captured.
    const float content = static_cast<float>(_buf.rec_size());

    // SOURCE maps into [0, content) -- the spec's half-open interval, and the
    // `- 1.f` is load-bearing, not cosmetic. Mapping SOURCE = 1.0 onto exactly
    // `content` puts the read position on the write head, and during recording
    // the two then advance at the identical rate (one write per sample, ratio
    // 1.0), so `frame == fsz` holds every sample and read_linear folds to 0
    // forever: the cloud goes near-silent at exactly SOURCE = 1.0 while
    // recording, and behaves normally a hair below it. Found in Task 3.
    const float span = content > 1.f ? content - 1.f : 0.f;

    // --- Rng draw order is contract: position, pan, octave, timing. ---
    const float jitter = _rng.next_bipolar() * motion * kScatterPosFrac * content;
    float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * span + jitter;
    while (centre >= content) centre -= content;
    while (centre < 0.f)      centre += content;

    const float pan = _rng.next_bipolar() * motion;

    const float ratio_base = _next_ratio();     // draws the octave roll

    // Spawn-timing jitter, applied to the NEXT interval. Drawn 4th.
    _spawn_jitter = _rng.next_bipolar() * motion * kScatterTimeFrac;

    // SUB: a share of grains an octave down. Drawn 5th.
    float ratio = ratio_base;
    if (_rng.next_unipolar() < _sub_n * kSubMaxShare) ratio *= 0.5f;

    // DTUN: per-grain detune spread, +-kDetuneCeilCt at full. Drawn 6th.
    const float cents = _rng.next_bipolar() * _detune_n * kDetuneCeilCt;
    if (cents != 0.f) ratio *= std::pow(2.f, cents / 1200.f);

    // Tape: the grain covers a fixed SIZE OF MATERIAL and so lasts
    // SIZE / ratio -- low notes smear long, high notes are fleeting.
    // Digital: fixed output duration; the grain reads SIZE * ratio.
    float lenf = _tape ? _grain_len / (ratio > 0.001f ? ratio : 0.001f)
                       : _grain_len;
    if (lenf < 2.f) lenf = 2.f;
    const int len = static_cast<int>(lenf);

    // ATK/DEC: the two window halves, each from kWindowHalfMin to
    // kWindowHalfMax of the grain. An unequal split IS the skew control.
    const float atk_f = lerpf(kWindowHalfMin, kWindowHalfMax, _atk_n);
    const float dec_f = lerpf(kWindowHalfMin, kWindowHalfMax, _dec_n);
    int atk = static_cast<int>(lenf * atk_f);
    int dec = static_cast<int>(lenf * dec_f);
    if (atk < 1) atk = 1;
    if (dec < 1) dec = 1;

    _grains[slot].spawn(centre, ratio, pan, len, atk, dec, _reverse);

    _last_ratio = ratio;
    _last_pan   = pan;
    _last_pos   = centre;
    _last_len   = len;
    ++_spawn_count;
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
            _spawn_ctr += _spawn_every * (1.f + _spawn_jitter);
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

    // Overlap normalization (1/sqrt(active)): the target is recomputed once
    // per control tick above, but applied here through a OnePole, advanced
    // every sample toward that tick-rate target -- same idiom as _level just
    // below. Without the smoothing the raw target steps the whole cloud
    // audibly on every grain spawn/retire.
    const float norm = _norm.process(_norm_target);
    l *= norm;
    r *= norm;

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

// --- voice row: setters below, the state they write is declared in the header ---
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
