#include "fx/dust.h"
#include "fx/flux.h"
#include <cmath>

using namespace spky;
using namespace spky::dust_tuning;

void DustCloud::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate > 0.f ? sample_rate : 48000.f;
    _rng.seed(seed);
    for (int i = 0; i < kGrains; ++i) _g[i] = Grain{};
    _dust = 0.f;
    _rot = 0.f;
    _grid_countdown = 1;
    // Preseed the zone-S grid to the 120 BPM value for this sample rate: the
    // transport's first real beat edge does not arrive until a full beat
    // after boot (0.5 s at 120 BPM), and _beat_samples defaulting to 0 would
    // leave zone S computing a zero-length grid (silent) until then -- an
    // audible missing first bar the moment a player turns DUST up before the
    // first beat lands. sync_beat() overwrites this the moment a real edge
    // arrives; nothing else depends on the specific default tempo.
    _beat_samples = 60.f / 120.f * _sr;
    _anchor = 0;
    // Start at the one-grain target, not 1.f: the smoother would otherwise
    // spend its first ~20 ms climbing the makeup, so the first grain after a
    // rise would come in 7 dB quiet.
    _norm = kGrainMakeup;
    _norm_coef = 1.f - std::exp(-1.f / (kNormSmoothS * _sr));
    _active_grains = 0;
    _curve = hann_curve().data();
    _remap();
}

void DustCloud::set_dust(float d) {
    d = clampf(d, 0.f, 1.f);
    // DUST returning to 0 must clear the pool, not merely stop advancing it:
    // process() early-returns at "_dust <= 0" BEFORE the grain loop, so every
    // `alive` grain would otherwise freeze exactly where it is -- alive, age,
    // absolute rd index and all -- and resume from that frozen state whenever
    // DUST next leaves zero, arbitrarily later, with the write head somewhere
    // else entirely. Callers (Flux::set_dust) already guard unchanged values
    // (I3), so a call here that actually changes `_dust` IS a real transition,
    // and comparing against the value being overwritten below is enough to
    // detect the 1 -> 0 edge without any extra state.
    if (_dust > 0.f && d <= 0.f) {
        for (int i = 0; i < kGrains; ++i) _g[i].alive = false;
        _norm = kGrainMakeup;   // one-grain target, see init()
        _grid_countdown = 1;
        // The pool is cleared, but `_anchor` itself is untouched -- left
        // pointing at whatever tape position it was latched to arbitrarily
        // long ago. Without a fresh latch request, the next zone-S sample
        // (whenever DUST next leaves zero) would spawn straight from that
        // stale anchor and replay up to a beat of off-grid material before
        // the following sync_beat() edge corrects it (task 12 finding 3).
        _beat_pending = true;
    }
    _dust = d;
    _remap();
}
void DustCloud::set_rot(float r)  { _rot  = clampf(r, 0.f, 1.f); _remap(); }

void DustCloud::sync_beat(float beat_samples) {
    // A NaN or non-positive length is bad input, not a real tempo: the
    // `(int32_t)` conversion of `_beat_samples / (1 << _subdiv)` in _remap()
    // below is undefined for a non-finite value, and callers besides
    // Instrument (tests, in particular) reach this API directly, without
    // Transport::set_bpm's guard two subsystems away (task 12 finding 4).
    // Dropped silently, same policy as Transport::set_bpm: the last good
    // beat length is kept rather than clamped to an arbitrary floor.
    if (!(beat_samples > 0.f) || !std::isfinite(beat_samples)) return;
    _beat_samples = beat_samples;
    _remap();
    _beat_pending = true;
}

void DustCloud::_remap() {
    const int prev_zone = _zone;   // task 12 finding 3: detect an F/R -> S crossing below

    // --- DUST: density, length, level, head takeover -----------------------
    const float d = _dust;
    const float rate_hz = kRateMin * std::pow(kRateMax / kRateMin, d);
    _birth_prob = rate_hz / _sr;
    _len_min = lerpf(kLenMinLo, kLenMinHi, d);
    _len_max = lerpf(kLenMaxLo, kLenMaxHi, d);

    // The normalisation is NOT computed here. It was 1/sqrt(expected overlap)
    // = 1/sqrt(rate_hz * mean_len), and that is wrong at the top of the knob —
    // design spec §2, corrected 2026-07-19. At DUST = 1 the tuning offers ~8.4
    // grains to a pool of 8; Erlang-B says a loss system at full utilisation
    // drops ~26 % of arrivals, so dividing by the OFFERED load made the cloud
    // QUIETER at maximum DUST (energy ratio 0.74 predicted, 0.68 measured).
    // The normalisation now follows the count actually sounding, in process().

    // Head takeover: above the knee the echo read head fades out (equal power)
    // so the cloud eats the delay. Feedback keeps recirculating underneath.
    if (d <= kTakeoverKnee) {
        _head_gain = 1.f;
    } else {
        const float t = (d - kTakeoverKnee) / (1.f - kTakeoverKnee);
        _head_gain = std::cos(t * 1.5707963705f);
    }

    // --- ROT: zone + within-zone morph -------------------------------------
    const float r = _rot;
    const float tape_max_s = (float)Flux::kMaxSamples / _sr;   // the FLUX
                                     // tape's actual length (~5.46 s @ 48 kHz),
                                     // not a bare 5.f (that literal used to
                                     // cover only 91.5 % of the real tape —
                                     // see design spec §8 Finding 1). Spray
                                     // target only — _spawn() clamps against
                                     // the real tape.size() regardless.
    if (r < kZoneSEnd) {                                // zone S — beat repeat
        _zone = 0;
        const float u = r / kZoneSEnd;
        _jitter = u;                                     // unchanged, ROT across the zone
        _subdiv = 1 + (int)(_dust * 3.999f);              // 1..4 => 2,4,8,16 slots/beat
        _octave = _dust >= kOctaveThresh;
        _rev_prob = 0.f;
        _wb_gain = 0.f;
        _wear = 1.f;
        // Grid comes from the BEAT, not the delay time (spec §3 rev 7): a
        // one-slot-per-beat grid would be a one-beat delay, which is exactly
        // what this zone must not be -- n = 0 is unreachable by construction
        // since _subdiv >= 1 always.
        const int32_t grid = (int32_t)(_beat_samples / (float)(1 << _subdiv));
        const int32_t grid_min = (int32_t)(kGridMinS * _sr);
        _grid_period = grid > grid_min ? grid : grid_min;
        // Upper-bound the grid so a slot can never outlive the tape it reads
        // from (task 12 finding 1). _spawn_anchored's `back = len * rate` is
        // clamped to `tape.size() - 4` as a last-resort safety net, but that
        // net alone lets grain LIFETIME (== _grid_period) run on long after
        // `back` has saturated: the octave (rate = 2) grain's read index then
        // walks past _anchor and out the other side, reading ahead of the
        // write head -- and the base (rate = 1) grain degenerates into a
        // fixed-offset delay tap, exactly what this zone exists to not be.
        // Bounding _grid_period at the source keeps `back` from ever needing
        // to saturate in the first place. The /2 is because the octave grain
        // traverses 2 * len; Flux::kMaxSamples (not a literal) is the actual
        // tape length every production TapeTap is built over -- see the
        // static_assert in fx/taps.cpp.
        constexpr int32_t kGridMax = (int32_t)((Flux::kMaxSamples - 4) / 2);
        if (_grid_period > kGridMax) _grid_period = kGridMax;
        if (_grid_countdown > _grid_period) _grid_countdown = _grid_period;
    } else if (r < kZoneFEnd) {                         // zone F — free scatter
        _zone = 1;
        const float u = (r - kZoneSEnd) / (kZoneFEnd - kZoneSEnd);
        _jitter = 1.f;
        _spray = (int32_t)(lerpf(kSprayFreeLo, tape_max_s, u) * _sr);
        _rev_prob = 0.f;
        _wb_gain = 0.f;
        _wear = 1.f;
    } else {                                            // zone R — rot
        _zone = 2;
        const float u = (r - kZoneFEnd) / (1.f - kZoneFEnd);
        _jitter = 1.f;
        _spray = (int32_t)(tape_max_s * _sr);
        _rev_prob = kRevProbMax * u;
        _wb_gain = kWbGainMax * u;
        _wear = 1.f - kWearRate * u;
    }

    // A ROT sweep INTO zone S from F or R (task 12 finding 3): `_anchor` was
    // last latched at whatever tape position the beat edge happened to land
    // on, possibly a long time ago while ROT was elsewhere entirely -- reuse
    // it as-is and the first zone-S sample after the sweep replays up to a
    // beat of material from a stale anchor, same failure as the DUST 0 ->
    // re-armed case in set_dust() above. Request the same fresh latch.
    if (prev_zone != 0 && _zone == 0) _beat_pending = true;
}

void DustCloud::_spawn(const TapeTap& tape) {
    for (int i = 0; i < kGrains; ++i) {
        if (_g[i].alive) continue;
        Grain& g = _g[i];

        const float lf = lerpf(_len_min, _len_max, _rng.next_unipolar());
        g.len = (int32_t)(lf * _sr);
        if (g.len < 8) g.len = 8;
        g.age = 0;

        const bool right = (_rng.next_u32() & 1u) != 0u;   // free stereo decorrelation
        // delta: 0 = forward, +2 = reverse (1x backwards). Task 2's SCOPE is
        // forward-only (reverse grains are Task 4) but this path is NOT dead:
        // _rev_prob is 0 outside zone R (ROT <= kZoneFEnd) but becomes > 0
        // inside it, and nothing stops a caller from reaching zone R with
        // Task 2 alone. So reverse grains are functional here whenever
        // ROT > 0.66 -- just unspecified and untested until Task 4 gives them
        // a real design (spray range, tape-overrun behaviour, etc. are only
        // sanity-clamped below, not tuned).
        const int32_t delta = (_rng.next_unipolar() < _rev_prob) ? 2 : 0;
        g.rd_step = delta - 1;

        // A reverse grain recedes 2 samples per sample; clamp the start so it
        // can never overrun the tape within its own lifetime.
        int32_t max_off = tape.size() - 4 - (delta ? 2 * g.len : 0);
        if (max_off < 2) max_off = 2;
        int32_t spray = _spray < max_off ? _spray : max_off;
        if (spray < 1) spray = 1;
        int32_t offset = 1 + (int32_t)(_rng.next_unipolar() * (float)spray);
        if (offset > max_off) offset = max_off;

        // Resolve the absolute read index once, at birth: (write_ptr +
        // offset), wrapped into [0, size) with the AND mask TapeTap's
        // power-of-two contract guarantees is exact (write_ptr and offset are
        // both non-negative here, so this needs no extra branch either).
        g.rd = (tape.write_ptr + offset) & tape.mask;
        g.tape = right ? tape.r : tape.l;

        const float pan = _rng.next_bipolar();
        const float a = (pan + 1.f) * 0.125f;      // -1..1 -> 0..0.25 turns
        g.gr = fast_sin(a);                        // equal-power, house idiom
        g.gl = fast_sin(a + 0.25f);

        // Hann window as a table index stepped once per sample and folded by
        // a sign flip at the midpoint (382 == 2 * (hann_curve().size() - 1)).
        g.widx = 0.f;
        g.wstep = 382.f / (float)g.len;
        g.hold  = 0;   // zones F/R: no flat top -- see process()'s fold comment

        g.alive = true;
        return;
    }
    // Pool full: the birth is dropped. This is what bounds the CPU (spec §8).
}

void DustCloud::_spawn_anchored(const TapeTap& tape, int rate) {
    for (int i = 0; i < kGrains; ++i) {
        if (_g[i].alive) continue;
        Grain& g = _g[i];

        g.len = _grid_period < 8 ? 8 : _grid_period;   // grain length = one slot
        g.age = 0;
        g.rd_step = -rate;

        // The write head decrements, so index _anchor + k is k samples OLDER
        // than the anchor. Starting `len * rate` behind the anchor and
        // stepping -rate walks the grain FORWARD in time, ending exactly at
        // _anchor -- every grain in the beat shares the same _anchor, so they
        // all replay the SAME slice (spec §3: the load-bearing property this
        // task exists to add).
        int32_t back = g.len * rate;
        if (back > tape.size() - 4) back = tape.size() - 4;
        g.rd = (_anchor + back) & tape.mask;

        // Pan, source channel and the pool-full drop are unchanged from
        // _spawn() above.
        const bool right = (_rng.next_u32() & 1u) != 0u;
        g.tape = right ? tape.r : tape.l;

        const float pan = _rng.next_bipolar();
        const float a = (pan + 1.f) * 0.125f;
        g.gr = fast_sin(a);
        g.gl = fast_sin(a + 0.25f);

        // Trapezoid gate (spec §3 rev 7): sin^2 ramps over a short fixed
        // fade, held flat between them -- a full-length Hann over a whole
        // slot would put a slow attack on every repeat and wash out the
        // transient that makes a repeat read as rhythm rather than a swell.
        int32_t fade = (int32_t)(kSlotFadeS * _sr);
        const int32_t half_len = g.len / 2;
        if (fade > half_len) fade = half_len;
        if (fade < 1) fade = 1;
        g.widx  = 0.f;
        g.wstep = 191.f / (float)fade;
        g.hold  = g.len - 2 * fade;

        g.alive = true;
        return;
    }
    // Pool full: the birth is dropped, same as _spawn() (spec §8).
}

void DustCloud::_schedule(const TapeTap& tape) {
    if (_zone != 0) {                       // zones F and R: free-running
        if (_rng.next_unipolar() < _birth_prob) _spawn(tape);
        return;
    }
    // Zone S: a beat repeat. Every slot fires (deterministic; DUST selects
    // the subdivision, not a probability -- spec §3 rev 7), phase-locked to
    // the transport's beat edge rather than duration-synced to the delay.
    if (_beat_pending) {              // consume the edge; latch the slice
        _beat_pending = false;
        _anchor = tape.write_ptr;
        _grid_countdown = 1;          // first slot fires on this sample
    }
    if (_grid_period < 1) return;
    if (--_grid_countdown > 0) return;

    _grid_countdown = _grid_period;
    if (_jitter > 0.f) {               // unchanged jitter on the countdown
        const float j = _rng.next_bipolar() * _jitter * 0.5f;
        _grid_countdown += (int32_t)(j * (float)_grid_period);
        if (_grid_countdown < 1) _grid_countdown = 1;
    }
    _spawn_anchored(tape, 1);
    if (_octave) _spawn_anchored(tape, 2);
}

float DustCloud::process(const TapeTap& tape, float& gl, float& gr) {
    gl = 0.f;
    gr = 0.f;
    if (_dust <= 0.f) return 0.f;

    _schedule(tape);

    const float* curve = _curve;   // cached in init(): no per-sample statics guard
    float sl = 0.f, sr = 0.f;
    int active = 0;
    for (int i = 0; i < kGrains; ++i) {
        Grain& g = _g[i];
        if (!g.alive) continue;
        ++active;

        int wi = (int)g.widx;
        if (wi > 190) wi = 190;
        if (wi < 0)   wi = 0;
        const float f = g.widx - (float)wi;
        const float w = curve[wi] + (curve[wi + 1] - curve[wi]) * f;

        const float s = g.tape[g.rd] * w;
        sl += s * g.gl;
        sr += s * g.gr;

        // Single AND wrap, matching bench/workloads_dust.cpp proc_dust_opt:
        // TapeTap::mask is a power-of-two contract now (see fx/dust.h), so
        // there is no longer a runtime case where a mask would be wrong.
        g.rd = (g.rd + g.rd_step) & tape.mask;

        g.widx += g.wstep;
        if (g.widx > 191.f) {
            if (g.hold > 0) { g.widx = 191.f; --g.hold; }   // flat top (zone S)
            else { g.widx = 382.f - g.widx; g.wstep = -g.wstep; }
        }

        if (++g.age >= g.len) g.alive = false;
    }

    // Normalise by what is ACTUALLY sounding, not by the expected overlap.
    // `active` was counted for free while iterating the pool above. The
    // reciprocal square roots come from a kGrains+1 entry constexpr table: a
    // per-sample sqrt + divide would cost ~30 cycles per part, and this
    // feature's budget was measured without one.
    _active_grains = active;
    // kGrainMakeup folds into the target, so it costs nothing per sample: the
    // window and pan losses it compensates are constant, so they belong in the
    // smoothed value rather than in a second multiply on the output.
    const float norm_target = kInvSqrt[active < 1 ? 1 : active] * kGrainMakeup;
    _norm += (norm_target - _norm) * _norm_coef;   // one multiply-add
    gl = sl * _norm;
    gr = sr * _norm;
    return 0.f;   // writeback arrives in Task 5
}
