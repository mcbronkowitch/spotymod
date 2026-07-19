#include "fx/dust.h"
#include <cmath>

using namespace spky;
using namespace spky::dust_tuning;

void DustCloud::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate > 0.f ? sample_rate : 48000.f;
    _rng.seed(seed);
    for (int i = 0; i < kGrains; ++i) _g[i] = Grain{};
    _dust = 0.f;
    _rot = 0.f;
    _delay_time = 0.5f;
    _grid_countdown = 1;
    _norm = 1.f;
    _norm_coef = 1.f - std::exp(-1.f / (kNormSmoothS * _sr));
    _active_grains = 0;
    _curve = hann_curve().data();
    _remap();
}

void DustCloud::set_dust(float d) { _dust = clampf(d, 0.f, 1.f); _remap(); }
void DustCloud::set_rot(float r)  { _rot  = clampf(r, 0.f, 1.f); _remap(); }

void DustCloud::set_delay_time(float s) {
    _delay_time = s > kGridMinS ? s : kGridMinS;
    _remap();
}

void DustCloud::_remap() {
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
    const float tape_max_s = 5.f;   // the FLUX tape (~5.46 s); spray target only —
                                     // _spawn() clamps against the real tape.size
    if (r < kZoneSEnd) {                                // zone S — synced stutter
        _zone = 0;
        const float u = r / kZoneSEnd;
        _jitter = u;
        _fire_prob = d;
        _burst = 1 + (int)(d * 2.f);                    // density stacks bursts
        _spray = (int32_t)(kSpraySync * _sr);
        _rev_prob = 0.f;
        _wb_gain = 0.f;
        _wear = 1.f;
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

    const int32_t grid = (int32_t)((_delay_time / (float)kGridDiv) * _sr);
    const int32_t grid_min = (int32_t)(kGridMinS * _sr);
    _grid_period = grid > grid_min ? grid : grid_min;
    if (_grid_countdown > _grid_period) _grid_countdown = _grid_period;
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
        int32_t max_off = tape.size - 4 - (delta ? 2 * g.len : 0);
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

        g.alive = true;
        return;
    }
    // Pool full: the birth is dropped. This is what bounds the CPU (spec §8).
}

void DustCloud::_schedule(const TapeTap& tape) {
    if (_rng.next_unipolar() < _birth_prob) _spawn(tape);
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
        // TapeTap::size is a power-of-two contract now (see fx/dust.h), so
        // there is no longer a runtime case where a mask would be wrong.
        g.rd = (g.rd + g.rd_step) & tape.mask;

        g.widx += g.wstep;
        if (g.widx > 191.f) { g.widx = 382.f - g.widx; g.wstep = -g.wstep; }

        if (++g.age >= g.len) g.alive = false;
    }

    // Normalise by what is ACTUALLY sounding, not by the expected overlap.
    // `active` was counted for free while iterating the pool above. The
    // reciprocal square roots come from a kGrains+1 entry constexpr table: a
    // per-sample sqrt + divide would cost ~30 cycles per part, and this
    // feature's budget was measured without one.
    _active_grains = active;
    const float norm_target = kInvSqrt[active < 1 ? 1 : active];
    _norm += (norm_target - _norm) * _norm_coef;   // one multiply-add
    gl = sl * _norm;
    gr = sr * _norm;
    return 0.f;   // writeback arrives in Task 5
}
