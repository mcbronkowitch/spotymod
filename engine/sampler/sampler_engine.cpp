#include "sampler/sampler_engine.h"
#include "util/math.h"
#include <cmath>

namespace spky {

namespace {
using namespace sampler_cfg;

// Pitch: the lane arrives already quantized from Part. The middle half of
// travel is the M5a mapping 8^(p-0.5) unchanged (+-9 semitones); both outer
// quarters steepen to reach +-kPitchOctaves at the ends. Control rate only
// -- _next_ratio reads the precomputed _chord_ratio cache, never this.
inline float ratio_for(float pitch_norm) {
    const float p = clampf(pitch_norm, 0.f, 1.f);
    if (p < kPitchKneeLo) {
        const float at_knee = std::pow(8.f, kPitchKneeLo - 0.5f);
        const float floor_r = std::pow(2.f, -kPitchOctaves);
        return floor_r * std::pow(at_knee / floor_r, p / kPitchKneeLo);
    }
    if (p > kPitchKneeHi) {
        const float at_knee = std::pow(8.f, kPitchKneeHi - 0.5f);
        const float ceil_r  = std::pow(2.f, kPitchOctaves);
        return at_knee * std::pow(ceil_r / at_knee,
                                  (p - kPitchKneeHi) / (1.f - kPitchKneeHi));
    }
    return std::pow(8.f, p - 0.5f);   // the M5a curve, unchanged
}
// Control rate only -- called once per control tick from _update_control, so
// the std::pow calls here are fine where they would not be in _spawn_one.
inline float size_seconds(float n) {
    n = clampf(n, 0.f, 1.f);
    if (n < kSizeKneeLo) {
        const float at_knee = kSizeMinS * std::pow(kSizeRange, kSizeKneeLo);
        return kSizeFloorS * std::pow(at_knee / kSizeFloorS, n / kSizeKneeLo);
    }
    if (n > kSizeKneeHi) {
        const float at_knee = kSizeMinS * std::pow(kSizeRange, kSizeKneeHi);
        return at_knee * std::pow(kSizeCeilS / at_knee,
                                  (n - kSizeKneeHi) / (1.f - kSizeKneeHi));
    }
    return kSizeMinS * std::pow(kSizeRange, n);   // the M5a curve, unchanged
}
// SCAN: knob position -> playhead rate in frames per sample (i.e. multiples
// of realtime). Control rate only -- std::pow must never reach the per-sample
// path. See the curve comment in sampler_config.h.
inline float scan_rate(float n) {
    n = clampf(n, -1.f, 1.f);
    const float a = n < 0.f ? -n : n;
    if (a < kScanDead) return 0.f;
    const float sign = n < 0.f ? -1.f : 1.f;
    if (a <= kScanKnee) {
        const float t = (a - kScanDead) / (kScanKnee - kScanDead);
        return sign * kScanMinRate * std::pow(1.f / kScanMinRate, t);
    }
    const float t = (a - kScanKnee) / (1.f - kScanKnee);
    return sign * lerpf(1.f, kScanMaxRate, t);
}
inline float cutoff_hz(float n) {
    return kCutoffMinHz * std::pow(kCutoffMaxHz / kCutoffMinHz, clampf(n, 0.f, 1.f));
}

// 2^(cents/1200) without std::pow, for the grain-spawn path. DTUN is bounded
// to +-kDetuneCeilCt cents, so the argument x = cents/1200 never leaves
// [-0.03, 0.03] and a cubic Taylor expansion of 2^x about 0 is exact to
// about 7e-9 relative -- far below a float's precision, let alone hearing.
// Coefficients are ln2, ln2^2/2, ln2^3/6.
inline float detune_factor(float cents) {
    const float x = cents * (1.f / 1200.f);
    return 1.f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f));
}

// The spawn interval, with its CPU floor. Extracted so the floor can be
// tested at chosen overlaps. That mattered at kOverlap = 4, where the
// shortest grain the SIZE curve can ask for (1 ms, 48 samples at 48 kHz)
// already yielded 12 samples and nothing here ever fired. At today's
// kOverlap = 8 it does fire on that same grain -- 48 / 8 = 6, lifted to
// kSpawnMinSamples -- so this floor is now live on the ordinary path.
inline float spawn_interval(float grain_len, float overlap) {
    const float raw = grain_len / (overlap > 0.f ? overlap : 1.f);
    return raw < kSpawnMinSamples ? kSpawnMinSamples : raw;
}
}  // namespace

float test_detune_factor(float cents) { return detune_factor(cents); }
float test_size_seconds(float n) { return size_seconds(n); }
float test_spawn_interval(float grain_len, int overlap) { return spawn_interval(grain_len, overlap); }
float test_ratio_for(float pitch_norm) { return ratio_for(pitch_norm); }
float test_scan_rate(float n) { return scan_rate(n); }

void SamplerEngine::init(float sample_rate) {
    _sr = sample_rate;
    _buf.init(_mem, _mem_frames, sample_rate);
    _slices.init(sample_rate);
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
    _svf_l.SetRes(_res_n);   // no SetDrive: SvfLp has no drive term (svf_lp.h)
    _svf_r.SetRes(_res_n);

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
    } else if (!_flow) {
        // Start the burst on the edge, not up to _spawn_every samples late:
        // leaving FLOW mid-cycle (or a prior STEP burst) can leave _spawn_ctr
        // anywhere in [0, _spawn_every), and STEP is supposed to reproduce
        // the phrase generator's composed rhythm exactly.
        //
        // Nur ausserhalb des FLOW. Im FLOW laeuft der Scheduler bereits, und
        // Part liefert dort trotzdem eine steigende Flanke pro PITCH-Zyklus
        // (part.cpp:226-229 setzt _gate_ctr ohne STEP-Pruefung). Jede davon
        // erzwang einen Sofort-Spawn und haengte die Wolkendichte an den
        // Phrasenrhythmus statt an DENS: bei SIZE 1.0 / DENS min sind das 50
        // Spawns in 10 s gegen den einen, den das 42-s-Intervall vorsieht.
        // Die untere DENS-Haelfte war dadurch praktisch wirkungslos.
        _spawn_ctr = 0.f;
    }
}

void SamplerEngine::process_in(float inL, float inR) {
    _in_l = inL;
    _in_r = inR;
    const size_t head = _buf.write_head();
    _buf.write(inL, inR);              // no-op unless recording
    // is_recording() before the call is NOT proof a frame landed at `head`:
    // write()'s fadeout early-return (sample_buffer.cpp, _fade_ctr == 0 on
    // entry -- two REC toggles inside one block) can leave is_recording()
    // true on entry yet write nothing. Check instead whether the head
    // actually moved off the snapshot across the call -- that IS landing.
    // "!=" and not ">": a ring wrap moves the head from the last frame back
    // to 0, which a naive `>` would miss.
    // Detect on what actually LANDED (post overdub mix), not on the input --
    // read back the frame the head just covered.
    if (_buf.valid() && _buf.write_head() != head) {
        const SampleBuffer::Frame& f = _buf.raw()[head];
        _slices.on_write(head, f.l, f.r);
    }
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
    _slices.scan(dst, n);
}

void SamplerEngine::trigger(float pitch_norm) {
    _burst_pitch   = pitch_norm;
    _burst_ratio   = ratio_for(_burst_pitch);   // control-rate: std::pow is fine here
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
    _burst_ratio   = ratio_for(_burst_pitch);   // control-rate: std::pow is fine here
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
    // The playhead goes home with the grains. This covers init(), clear() and
    // load_sample() in one place -- they are _kill_all's only three callers --
    // so no reset path can be forgotten. punch() deliberately does NOT come
    // through here: it rewinds the head without killing what is sounding.
    _scan_pos = 0.f;
}

// SIZE is asymmetrically live: down cuts, up does not stretch.
//
// Why this exists at all: Grain latches its length at spawn, so before this
// a grain spawned at SIZE 1.0 sounded for its full 42 s (kSizeCeilS) however
// far the knob came back down -- 84 s under the tape pool ceiling, 87 s at
// kGrainLenCeil -- and no control on the deck could stop it. _release_all is
// reachable only from set_hold, which only the OTHER part's CHOKE window
// drives (instrument.cpp:110); leaving FLOW deliberately does not release
// (grain.h). SIZE therefore had a settling time of up to 42 s during which
// the knob and what you heard disagreed.
//
// The rule: rescale each running grain to the length it WOULD have been given
// at today's SIZE -- its spawn length times (SIZE now / SIZE then) -- and cap
// it there. Three consequences worth stating:
//
//   * Only shortening. trim_total ignores a cap looser than what is left, so
//     SIZE up is a no-op on running grains and the cloud still drags behind
//     a rising lane.
//   * Tape keeps its smear. The quotient is taken against _grain_len, not
//     the knob, and a tape grain's len already contains 1 / ratio, so the
//     proportion survives the rescale instead of being clipped to SIZE.
//   * Idempotent, so a lane modulating SIZE does not compound. Scaling the
//     REMAINING life by (new / previous) each tick would telescope over an
//     LFO cycle and collapse the cloud in seconds; scaling the TOTAL against
//     the spawn-time size depends only on where SIZE is now.
//
// Runs on the control raster, kGrains iterations of a compare -- the loop is
// skipped entirely for slots that are not sounding.
void SamplerEngine::_trim_running() {
    const int min_fade = static_cast<int>(kRecordFade);
    for (int i = 0; i < kGrains; ++i) {
        if (!_grains[i].active()) continue;
        const float ref = _size_ref[i];
        if (!(ref > 0.f) || _grain_len >= ref) continue;   // SIZE did not fall
        const float scaled = _len_ref[i] * (_grain_len / ref);
        int total = scaled > 1.f ? static_cast<int>(scaled) : 1;
        _grains[i].trim_total(total, min_fade);
    }
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
// MOTION used to add an octave scatter here: a per-grain roll against
// kScatterOctProb that multiplied the ratio by 0.5 or 2. It is gone, and
// deliberately so.
//
// The morphagene-controls work (spec 2026-07-21) stopped the PITCH lane from
// modulating the sampler, because the instrument's author needs a sampler deck
// to hold still against a synth deck playing in the same key. That closed the
// melody door but not this one: the octave roll rides LANE_MOTION, which the
// VCV host never writes for a sampler part, so it sat on Part's default base
// of 0.5 (part.h:238) even with MOD at zero -- kScatterOctProb * 0.5 == 12.5%
// of every grain jumping a full octave. Measured on the author's own panel
// setting: 311 spawns, ratios {1.0, 2.0, 0.5}. The stated requirement was not
// met, and no amount of knob-setting could meet it.
//
// MOTION keeps its other three scatters (position, spawn timing, pan) --
// none of them touch pitch, and they carry the character on their own.
// This is sampler-only code; SynthEngine has its own path and is untouched.
//
// Both Rng draws went with it, not just the multiply. Keeping a dead draw to
// preserve the stream position would only freeze an arbitrary alignment of the
// following draws (timing, sub, detune) -- the sampler has no bit-identity
// promise to keep here, and its listening scenarios pin no hashes.
float SamplerEngine::_next_ratio() {
    const bool latched = (!_flow && _burst_latched);

    float ratio;
    if (latched && _chord_n <= 1) {
        // Frozen at the trigger that set it, not _chord_ratio[0] -- see the
        // comment at _burst_ratio's declaration for why these are two caches
        // and not one.
        ratio = _burst_ratio;
    } else {
        // COLOR > 0 (_chord_n > 1) takes this branch even during a latched
        // STEP burst, and reads _chord_ratio[] live rather than a value
        // frozen at trigger time. Part::_control_tick refreshes _chord[]
        // (and _update_control refreshes _chord_ratio[] from it) every 96
        // samples, so the note set a round-robin burst is drawing from can
        // change mid-burst if the composed chord changes underneath it. The
        // spec (sampler-texture-deck-design.md) says trigger "latches the
        // pitch (or chord set) for the burst" -- for chords (COLOR > 0) the
        // current code does not do that; only the single-note path above
        // (_chord_n <= 1) is actually latched. This is a known deviation,
        // not a bug to silently fix: whether a burst should freeze its whole
        // chord set at the gate edge is a musical call for the instrument's
        // author, not an engine-layer decision.
        const int idx = _rr % _chord_n;   // capture before _rr advances
        _rr = (_rr + 1) % _chord_n;
        ratio = _chord_ratio[idx];
    }

    return ratio;
}

float SamplerEngine::_next_interval() const {
    const float n = _spawn_every * (1.f + _spawn_jitter);
    return n < kSpawnMinSamples ? kSpawnMinSamples : n;
}

void SamplerEngine::_update_control() {
    // --- SIZE: piecewise exponential, 1 ms .. 42 s ---
    // No clamp to content length. read_linear folds modulo the recorded
    // length (sample_buffer.cpp:184-190), so a grain longer than its
    // material is a loop with a window drawn over it -- a slow swell, which
    // is the musical point of the top of the knob rather than a defect.
    float len = size_seconds(_targets[LANE_SIZE]) * _sr;
    if (len < 2.f) len = 2.f;             // Grain::spawn's own safety minimum
    _grain_len = len;

    // Before anything derived is recomputed: what is already sounding gets
    // rescaled to the new SIZE, downward only. See _trim_running.
    _trim_running();

    // The CPU floor lives HERE, on the interval, not on _grain_len. The cost
    // this guards is per spawn, and kOverlap decouples the two: a length
    // floor stops bounding the spawn rate as soon as kOverlap rises. See
    // kSpawnMinSamples.
    _spawn_every = spawn_interval(_grain_len, _overlap);

    // A shrinking interval must not leave a stale long countdown pending:
    // sweeping SIZE down would otherwise gap the carpet for up to the old
    // interval while grains retire at the new, much shorter length.
    //
    // Gegen _next_interval() und NICHT gegen _spawn_every: der laufende
    // Countdown traegt den Timing-Jitter, und ein Clamp auf das ungejitterte
    // Grundintervall schneidet jedes zu lange Intervall weg, waehrend jedes
    // zu kurze stehenbleibt. Aus einem symmetrischen Jitter wird so eine
    // einseitige Beschleunigung. _next_interval() folgt SIZE- und
    // DENS-Aenderungen ueber _spawn_every weiterhin sofort, der Zweck dieser
    // Zeile bleibt also erhalten.
    const float ceiling = _next_interval();
    if (_spawn_ctr > ceiling) _spawn_ctr = ceiling;

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

    // --- SCAN: advance the playhead one control tick's worth ---
    // Control rate, not per sample: kCtrlInterval is 96 samples (~2 ms), far
    // finer than any audible playhead motion, and it keeps std::floor off the
    // per-sample path. The fold is the same O(1) form read_linear uses
    // (sample_buffer.cpp) -- deliberately NOT a subtract loop, which would
    // spin once per content length after a long freeze at a high rate.
    // An empty buffer parks the head: there is nothing to read, and a
    // position accumulated against no content would be meaningless the moment
    // a recording started.
    const float scan_content = static_cast<float>(_buf.rec_size());
    if (scan_content > 0.f) {
        _scan_pos += _scan_rate * static_cast<float>(kCtrlInterval);
        if (_buf.is_recording()) {
            // Author's fixed-lag decision (spec 2026-07-21 morphagene-
            // controls), NOT the ordinary fold below. Content grows by one
            // frame per sample while recording; SCAN at realtime forward
            // grows _scan_pos by that same amount from the same zero start,
            // so _scan_pos == content on every control tick and the fold
            // resets to 0 every time -- the head is pinned still, not
            // running independently, which contradicts the spec's promise
            // that record and playback heads are separate. Clamping instead
            // of folding holds the read position a fixed kScanRecordLagS
            // behind the write head: at realtime forward the clamp engages
            // every tick and the head rides the lag; slower than realtime it
            // falls further behind on its own and the clamp goes slack;
            // faster, it cannot overtake the write head and sticks at the
            // lag. Also the fix for the near-silence _spawn_one's own
            // `span = content - 1.f` comment documents: reading exactly on
            // the write head is what that comment warns against, and
            // realtime-forward SCAN during a recording is exactly that case
            // under the fold.
            //
            // is_recording() is a State enum compare (sample_buffer.h) --
            // free at control rate.
            const float lag = kScanRecordLagS * _sr;
            const float ceiling = scan_content - lag;   // negative early on
            if (_scan_pos > ceiling) _scan_pos = ceiling;
            // Floor at 0 separately from the ceiling clamp above: early in a
            // recording (content < lag) ceiling itself is negative, so
            // clamping to it alone would drive _scan_pos negative instead of
            // parking it at the start of the little content that exists.
            if (_scan_pos < 0.f) _scan_pos = 0.f;
        } else {
            _scan_pos -= scan_content * std::floor(_scan_pos / scan_content);
            // Both ends, and the upper one is the one that actually fires: in
            // float32 a _scan_pos just below zero folds to EXACTLY scan_content,
            // not to something just under it. That is the value _spawn_one's own
            // `span = content - 1.f` comment calls out as dangerous, and it would
            // break the [0, rec_size) contract that scan_pos() promises to its
            // callers (sampler_scan_pos(), read directly by tests/
            // test_scenario.cpp). The lower branch
            // is not provably unreachable in exact arithmetic -- it stays only as
            // a cheap belt against float rounding in the subtract-fold above --
            // but it is unreachable in practice: _scan_rate (set_scan) is either
            // exactly 0.f (the SCAN dead zone) or at least kScanMinRate in
            // magnitude, never an arbitrarily small nonzero value, so _scan_pos
            // always steps by a non-negligible amount and the fold has no
            // denormal-sized gap to land short of zero in.
            if (_scan_pos >= scan_content || _scan_pos < 0.f) _scan_pos = 0.f;
        }
    } else {
        _scan_pos = 0.f;
    }

    // --- FILT: same bipolar rails as the synth; set_filt() is the knob ---
    const float off   = _filt_amt < 0.f ? kFiltLeftScale * _filt_amt : _filt_amt;
    const float n_raw = kFiltNeutral + off;
    _filt_gain = clampf(1.f + n_raw / kFiltFadeRange, 0.f, 1.f);
    const float hz = cutoff_hz(n_raw);
    _svf_l.SetFreq(clampf(hz, 20.f, 0.3f * _sr));
    _svf_r.SetFreq(clampf(hz, 20.f, 0.3f * _sr));

    // Chord ratios, precomputed at control rate. _chord[] is refreshed by
    // Part::_control_tick on this same 96-sample tick, so this cache is
    // never stale by more than one tick -- the same freshness the chord
    // itself has. Feeds only _next_ratio's round-robin (COLOR > 0) branch;
    // the latched single-note branch has its own cache, _burst_ratio, set at
    // trigger time -- see the comment at _burst_ratio's declaration.
    const int n_notes = _chord_n > 0 ? _chord_n : 1;
    for (int i = 0; i < n_notes; ++i) _chord_ratio[i] = ratio_for(_chord[i]);
}

void SamplerEngine::_spawn_one() {
    if (_buf.is_empty()) return;

    // Ein Durchlauf, zwei Antworten: der erste freie Slot und wieviele Grains
    // gerade klingen. Der Zaehler ist gratis -- die Schleife lief ohnehin --
    // und ohne ihn bräuchte der Deckel unten ein zweites active_grains().
    int slot = -1;
    int live = 0;
    for (int i = 0; i < kGrains; ++i) {
        if (_grains[i].active())      ++live;
        else if (slot < 0)            slot = i;
    }
    const float motion  = clampf(_targets[LANE_MOTION], 0.f, 1.f);

    // Dichtedeckel (kSpawnHeadroom). Der Pool ist mit kGrains == 2 *
    // kOverlapMax bewusst doppelt so gross wie der hoechste angeforderte
    // Overlap, damit der Timing-Jitter Grains stapeln kann, ohne Spawns zu
    // verlieren. Genau dieses Stapeln ist aber der teuerste Block: gelesen
    // wird pro Sample einmal je klingendem Grain, und deren Zahl schwankt bei
    // MOTION 1 um Faktor 1.36 statt um 1.01 (Messung in sampler_config.h bei
    // kSpawnHeadroom).
    //
    // Der Deckel haengt an _overlap und nicht an kGrains, weil DENS die
    // angeforderte Dichte IST: wer DENS herunterdreht, bekommt auch den
    // billigeren Worst Case, statt weiter fuer die Pooldecke zu bezahlen.
    // std::ceil, damit ein gebrochener Overlap (DENS ist stufenlos) nach oben
    // aufgeht und der Deckel nie unter die bestellte Dichte faellt.
    const int ceiling = static_cast<int>(std::ceil(_overlap)) + kSpawnHeadroom;
    const bool full   = slot < 0;
    const bool capped = live >= (ceiling < kGrains ? ceiling : kGrains);
    if (full || capped) {
        ++_dropped_spawns;
        // Den Timing-Jitter zuruecksetzen, und das ist keine Kosmetik:
        // _next_interval() rechnet mit _spawn_jitter, und wer ihn beim
        // Fallenlassen STEHEN laesst, wiederholt bei einem negativen Wert
        // dasselbe zu kurze Intervall, bis endlich ein Spawn durchkommt. Aus
        // einem symmetrisch gezogenen Jitter wird so eine einseitige
        // Beschleunigung -- gemessen +3.5 % ueber der nominellen Spawnrate,
        // was "F-01: the spawn rate matches nominal even with timing jitter"
        // prompt gefangen hat. Dieselbe Falle wie beim Clamp in
        // _update_control, nur an der anderen Stelle.
        //
        // Auf 0 und NICHT neu gezogen, obwohl ein frischer Zug die
        // natuerlichere Antwort waere: ein Zug hier verbraucht einen
        // Rng-Wert, und weil ein gefallener Spawn sonst gar keinen
        // verbraucht, verschiebt das ab dem ersten Drop die gesamte
        // Zugfolge -- der gelockte Golden Vector ("Rng draw order and SOURCE
        // mapping are locked") bricht dann ab Spawn 9. 0 ist erwartungstreu
        // (E[Intervall] = _spawn_every, genau wie ueber die gejitterte
        // Verteilung), kostet keinen Zufall, und die dokumentierte
        // Zugreihenfolge unten bleibt Zug fuer Zug dieselbe: ein gefallener
        // Spawn ueberspringt sie vollstaendig, statt sie zu verschieben.
        _spawn_jitter = 0.f;
        return;
    }
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

    // --- Rng draw order is contract: position, pan, timing, sub, detune. ---
    // (The octave roll and its direction draw used to sit between pan and
    // timing, inside _next_ratio; both were removed with the scatter itself.)
    const float jitter = _rng.next_bipolar() * motion * kScatterPosFrac * content;
    float centre = clampf(_targets[LANE_SOURCE], 0.f, 1.f) * span + _scan_pos + jitter;
    while (centre >= content) centre -= content;
    while (centre < 0.f)      centre += content;

    const float pan = _rng.next_bipolar() * motion;

    const float ratio_base = _next_ratio();     // draws nothing from the Rng

    // Spawn-timing jitter, applied to the NEXT interval. Drawn 3rd.
    _spawn_jitter = _rng.next_bipolar() * motion * kScatterTimeFrac;

    // SUB: a share of grains an octave down. Drawn 4th.
    float ratio = ratio_base;
    if (_rng.next_unipolar() < _sub_n * kSubMaxShare) ratio *= 0.5f;

    // DTUN: per-grain detune spread, +-kDetuneCeilCt at full. Drawn 5th.
    const float cents = _rng.next_bipolar() * _detune_n * kDetuneCeilCt;
    if (cents != 0.f) ratio *= detune_factor(cents);

    // Tape: the grain covers a fixed SIZE OF MATERIAL and so lasts
    // SIZE / ratio -- low notes smear long, high notes are fleeting.
    // Digital: fixed output duration; the grain reads SIZE * ratio.
    float lenf = _tape ? _grain_len / (ratio > 0.001f ? ratio : 0.001f)
                       : _grain_len;

    // Pool-throughput bound. A grain must not outlive the time it takes to
    // fill every slot, because spawns past that point have nowhere to go.
    //
    // Without this, tape at the top of SIZE starves the cloud for the better
    // part of an hour. _grain_len at SIZE 1.0 is 42 s (2,016,000 samples) and
    // the composed minimum ratio is 2^-4 (pitch) x 0.5 (SUB) x 0.983 (detune)
    // ~= 0.0307, so lenf reaches ~6.6e7 samples -- 22.8 minutes. (Before the
    // MOTION octave scatter was removed there was another x 0.5 in that
    // product: minimum ratio 0.0154, lenf ~1.31e8, 45.6 minutes. The bound
    // below does not depend on either figure -- removing a term only made the
    // worst case milder.) _spawn_every is 2,016,000 / 8 = 252,000, so all kGrains
    // slots fill after 84 s and every spawn from then on is dropped, with no
    // knob that recovers it: length is latched at spawn and only CHOKE /
    // set_hold (_release_all) frees a slot early. Measured directly at PITCH
    // minimum alone (ratio 0.0625, lenf 32,256,000): spawn_count froze at 16
    // and 8 of 24 attempts were dropped over 125 s.
    //
    // Wo die Decke WIRKLICH bindet, und das ist nicht der Extremfall oben:
    // bei DENS max ist _spawn_every = _grain_len / 8, also len_ceil =
    // 2 * _grain_len. Tape gibt lenf = _grain_len / ratio, und das erreicht
    // die Decke schon bei ratio = 0.5 -- bei JEDER Transposition ueber eine
    // Oktave abwaerts, bei ganz normalem SIZE. Die Tape-Zusage "low notes
    // smear long" endet dort still: ab einer Oktave abwaerts wird das Grain
    // nicht laenger, sondern gekappt (gepinnt von "F-10: the tape ceiling
    // binds at one octave down" in tests/test_sampler_engine.cpp).
    //
    // Bewusst so belassen. Was es loesen wuerde, ist Voice-Stealing -- das
    // aelteste Grain verdraengen statt den Spawn fallenzulassen -- und das
    // ist eine breite Verhaltensaenderung am haeufigen dichten Pfad fuer
    // einen Gewinn am duennen Rand. Siehe den Absatz zum sparse case unten.
    //
    // The bound is self-adjusting to any kOverlap / kGrains and needs no new
    // tuning constant. It is not free, though, and the comment should own
    // that: in a SPARSE spawn pattern -- a short STEP burst, or FLOW switched
    // off right after a spawn -- nothing was contending for the slot, so the
    // grain it trims from 45 minutes to 84 s would genuinely have sounded for
    // longer. Accepted deliberately: 84 s is already far past any musical
    // grain, both figures read to the ear as "frozen", and the alternative
    // that would preserve the sparse case (stealing the oldest grain when the
    // pool is full instead of dropping) is a much wider behavioural change to
    // the common dense path for a gain only audible at an extreme nobody can
    // hear the difference within. See the final-fix report.
    //
    // Applied to BOTH modes rather than only the tape branch, because it is
    // provably a no-op in digital and that is cheaper than a branch: there
    // lenf == _grain_len, and _spawn_every == max(_grain_len / _overlap,
    // kSpawnMinSamples) >= _grain_len / _overlap, so len_ceil >= _grain_len *
    // kGrains / _overlap.
    //
    // _overlap is a runtime DENS value in [kOverlapMin, kOverlapMax] = [1, 8]
    // since Task 1 (sampler_config.h), not the kOverlap = 8 compile-time
    // constant this bound was first derived against -- kGrains is still
    // fixed at 16, though, so at _overlap == kOverlapMax the bound is
    // len_ceil >= _grain_len * kGrains / kOverlapMax == 2 * _grain_len (the
    // 2x figure kGrains == 2 * kOverlapMax was chosen to guarantee). Below
    // kOverlapMax, the same inequality only grows len_ceil's lower bound
    // (smaller _overlap, larger kGrains / _overlap), so turning DENS down
    // can only loosen this ceiling, never tighten it below that 2x floor --
    // see the "lowering overlap only loosens the pool ceiling" test in
    // test_sampler_engine.cpp. Writing the bound unconditionally also keeps
    // it true if a future pair ever sets kGrains <= kOverlapMax.
    // Gegen `ceiling` und nicht mehr gegen kGrains, seit der Dichtedeckel
    // oben existiert: die erreichbare Poolgroesse IST jetzt ceiling, und die
    // beiden Schranken muessen dieselbe Zahl meinen. Taten sie es nicht,
    // dimensionierte diese Zeile die Grainlaenge auf 16 gleichzeitig
    // klingende Grains, waehrend der Deckel nur 9 zulaesst -- und dann fiel
    // im Tape-Eck bei max SIZE und tiefer Transposition jeder Spawn ueber
    // dem Deckel, sogar bei MOTION 0, wo es gar keinen Jitter zu absorbieren
    // gibt. Genau das hat "sampler: tape mode at max SIZE and min pitch
    // keeps the cloud spawning" gefangen (8 Drops statt 0).
    //
    // Was das hoerbar kostet, und es gehoert benannt statt weggerechnet: die
    // Tape-Zusage "low notes smear long" wird kuerzer. Bei kSpawnHeadroom = 2
    // deckelt sie bei DENS max auf 1.25 statt 2 Grainlaengen (Smearing endet
    // rund vier Halbtoene abwaerts statt erst eine Oktave tiefer), bei DENS
    // min auf 3 statt 16. Die Wahl der 2 ist eine Hoerentscheidung und steht
    // mit ihrer Tabelle bei kSpawnHeadroom.
    // Das ist kein Nebeneffekt, den man wegdesignen koennte -- ein langes
    // Tape-Grain IST Dichte, und beides gleichzeitig zu deckeln und laufen
    // zu lassen ist dieselbe Resource zweimal vergeben. Die alte Fassung
    // liess im Tape-Eck genau die 2x-Dichtespitze zu, gegen die der Deckel
    // gebaut ist.
    const float len_ceil = _spawn_every * static_cast<float>(ceiling);
    if (lenf > len_ceil) lenf = len_ceil;

    // Zweite, absolute Decke. Die Pool-Decke oben skaliert mit 1 / _overlap
    // und wird bei DENS min viermal so gross wie 2^23 -- die Schranke, ab
    // der Grain::_off in float32 einfriert (32 256 000 gegen 8 388 608).
    // kGrainLenCeil ist NICHT diese Schranke, sondern 2^22 und damit
    // bewusst ihre Haelfte, als Reserve fuer die kleinsten erreichbaren
    // Ratios. Die Pool-Decke allein reicht jedenfalls nicht mehr, seit der
    // Overlap zur Laufzeit veraenderlich
    // ist -- was grain.h's Stall-Argument voraussetzt.
    if (lenf > kGrainLenCeil) lenf = kGrainLenCeil;

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
    // The pair _trim_running rescales against. Recording _grain_len rather
    // than the SIZE knob keeps tape mode honest: lenf there is _grain_len /
    // ratio, so the QUOTIENT len / _size_ref carries the mode, the pitch and
    // both ceilings, and rescaling by SIZE alone reproduces all of them.
    _size_ref[slot] = _grain_len;
    _len_ref[slot]  = static_cast<float>(len);

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
            _spawn_one();                    // zieht _spawn_jitter neu
            // _next_interval() bodet bereits auf kSpawnMinSamples, und
            // _spawn_ctr ist an dieser Stelle > -1, also bleibt die Summe
            // sicher positiv -- die alte `if (_spawn_ctr < 1.f)`-Klemme war
            // genau die Stelle, an der der Jitter den CPU-Boden unterlief.
            _spawn_ctr += _next_interval();
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
    // Measured, not guessed: an earlier version of this clamp (0.7) was
    // bisected against a peak threshold -- sweep FILT across its whole range
    // while resonating and require the output peak stay under some cap. That
    // was the wrong test. Peak-vs-resonance at a fixed sweep duration is a
    // smooth accelerating curve with no knee (10 s sweep: 0.7 -> 7.22,
    // 0.8 -> 11.11, 0.9 -> 21.20, 0.95 -> 34.59, 1.0 -> 72.77), so any peak
    // number you pick is arbitrary -- there is no threshold on that curve
    // that marks a regime change, and 0.70/0.72/0.75 (under 15% apart in
    // peak) are not distinguishable regimes.
    //
    // The real boundary is duration-dependence, which the FILT sweep is well
    // placed to expose since a self-oscillating SVF is likeliest to diverge
    // while its cutoff is moving. Below the boundary, the peak from a given
    // resonance barely moves as the same sweep runs 10x longer: 10 s -> 100 s
    // growth measures 0.85 -> 2.3%, 0.90 -> 7.0%, 0.92 -> 11.4%. Above it,
    // growth accelerates sharply: 0.95 -> 26.6%, 1.0 -> 210% (72.8 at 10 s to
    // 225.3 at 100 s, still climbing at 3000 s). That puts the boundary
    // between 0.90 and 0.95. "sampler: resonance ceiling is duration-stable,
    // not just finite" (tests/test_sampler_engine.cpp) asserts a 10 s and a
    // 100 s sweep agree within 10% at whatever resonance this clamps to; it
    // passes here with real margin (7.0% against the 10% tolerance) and
    // fails outright at 0.95. The clamp sits at 0.90 -- well above the old
    // 0.7, which was never testing the property that actually bounds this.
    //
    // Two honest qualifications, because the paragraph above overclaims by
    // omission if they are left out:
    //
    // 1. "The actual stability boundary" is too strong. Run the horizon out
    //    to 300-3000 s and 0.95 and 0.98 also plateau -- they settle at a
    //    higher level, they do not diverge. Only 1.0 is genuinely unbounded
    //    (still climbing at 3000 s). So 0.90 is a HEADROOM limit, chosen for
    //    how much level the SVF hands downstream and how fast it gets there,
    //    not a divergence limit. The thing that actually diverges is one
    //    value away, at the very top.
    //
    // 2. The rejection of peak thresholds as "an arbitrary line through a
    //    continuum" applies to the 10% growth tolerance too, and it would be
    //    dishonest to pretend otherwise. Growth-vs-resonance (2.3 / 7.0 /
    //    11.4 / 26.6%) is the same kind of smooth curve as peak-vs-resonance
    //    was; 0.92 fails only because the tolerance is written 10 rather than
    //    12. The genuine improvement over the peak test is not that the line
    //    stopped being arbitrary -- it is that the QUANTITY is the right one:
    //    duration-dependence is what distinguishes a filter that rings from
    //    one that runs away, where a peak at a fixed sweep length does not
    //    distinguish them at all. The line's exact position remains taste.
    _res_n = clampf(n, 0.f, 0.9f);
    _svf_l.SetRes(_res_n);
    _svf_r.SetRes(_res_n);
}
void SamplerEngine::set_sub(float n)    { _sub_n = clampf(n, 0.f, 1.f); }
void SamplerEngine::set_detune(float n) { _detune_n = clampf(n, 0.f, 1.f); }

void SamplerEngine::set_overlap(float n) {
    _overlap = lerpf(kOverlapMin, kOverlapMax, clampf(n, 0.f, 1.f));
}

void SamplerEngine::set_scan(float bipolar) { _scan_rate = scan_rate(bipolar); }

void SamplerEngine::punch() {
    _scan_pos  = 0.f;
    _spawn_ctr = 0.f;   // the next process() spawns; see the scheduling block
}

}  // namespace spky
