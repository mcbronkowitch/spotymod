#pragma once
#include <cstdint>
#include "fx/fx_util.h"
#include "mod/rng.h"
#include "util/fast_sin.h"
#include "util/math.h"

namespace spky {

// All DUST/ROT tuning lives here, in one block, so the deferred Rack play-test
// has a single place to turn. The spec fixes behavior, these fix taste.
namespace dust_tuning {
constexpr float kZoneSEnd = 0.33f;      // sync stutter -> free scatter
constexpr float kZoneFEnd = 0.66f;      // free scatter -> rot

constexpr float kRateMin  = 1.5f;       // births/s just above DUST = 0
constexpr float kRateMax  = 35.f;       // births/s at DUST = 1 (~8 overlap)
// Level normalisation follows the ACTIVE grain count, not the expected overlap
// (spec §2, corrected 2026-07-19: 1/sqrt(expected overlap) divides by load the
// 8-grain pool cannot carry once births are dropped near DUST = 1, which made
// the cloud quieter at maximum DUST instead of louder). Smoothed, because the
// raw count steps by +-1 on every birth and death and would pump at up to
// ~35 births/s.
constexpr float kNormSmoothS = 0.02f;   // ~20 ms; ear-tunable in the play-test

// Grain-path makeup. 1/sqrt(active) compensates for the grain COUNT, and for
// nothing else -- but every grain sample is also multiplied by the Hann window
// and by an equal-power pan gain, and both cost level:
//
//   Hann sin^2(pi t), RMS over a grain's life = sqrt(3/8) = 0.6124   -4.26 dB
//   equal-power pan, p uniform -> E[pan^2] = 0.5, RMS = 0.7071       -3.01 dB
//   combined                                        0.4330          -7.27 dB
//
// So without this the cloud sits 7.3 dB under a direct tape read at EVERY DUST
// setting, and the echo head -- which reads the tape at unity and then builds
// up through feedback on top -- buries it. Found by ear in the Phase A listen
// (2026-07-19): "the grains are very quiet next to the delay; only at DUST full
// do you hear them", which is exactly the head takeover removing the reference
// rather than the grains getting louder.
//
// 1/0.4330 puts one grain at parity with one direct tape read. It deliberately
// does NOT also compensate the echo's feedback build-up (1/(1-fb), another
// +5..+14 dB) -- that is the player's FLUX FEEDBACK setting, and matching it
// would make the cloud scream at low feedback.
constexpr float kGrainMakeup = 2.309f;  // +7.27 dB; ear-tunable

constexpr float kLenMinLo = 0.025f;     // grain length range at DUST = 0
constexpr float kLenMaxLo = 0.100f;
constexpr float kLenMinHi = 0.080f;     // ... and at DUST = 1
constexpr float kLenMaxHi = 0.400f;

constexpr float kSpraySync   = 0.05f;   // s, zone S: tight, near the head
constexpr float kSprayFreeLo = 0.15f;   // s, zone F start (widens to the tape)

constexpr int   kGridDiv    = 4;        // zone S grid = FLUX delay time / 4
constexpr float kGridMinS   = 0.010f;   // ... clamped to a sane minimum

constexpr float kRevProbMax = 0.70f;    // zone R reverse probability at r = 1
constexpr float kWbGainMax  = 0.60f;    // zone R writeback gain at r = 1
constexpr float kWearRate   = 4.0e-6f;  // per-sample erosion at r = 1

constexpr float kTakeoverKnee = 0.70f;  // DUST above this fades the echo head
}

// A read-only view of one part's stereo tape at this sample. No ownership, no
// virtuals, and tests can build one over a plain array. `mask` carries a
// power-of-two CONTRACT (mirroring DeLine/EchoDelay: production's FLUX tape is
// 262144 = Flux::kMaxSamples, already a power of two) so every wrap in the hot
// grain loop is a single AND, never a divide. A plain aggregate, deliberately:
// there used to be a separate `size` alongside `mask`, derived by a 4-arg
// constructor that asserted the power-of-two contract on every call —
// per-sample in the audio path once Flux::process constructs one of these
// each sample, and a size/mask pair that a default-constructed or partially
// written TapeTap could still get out of sync (size set, mask left 0). Now
// there is only `mask`; nothing to disagree with it. The power-of-two
// contract itself is checked once, at compile time, in dust.cpp
// (static_assert against Flux::kMaxSamples) rather than per construction.
struct TapeTap {
    const float* l = nullptr;
    const float* r = nullptr;
    int32_t write_ptr = 0;
    int32_t mask = 0;

    // Tape length, derived from mask. Only used at grain birth (_spawn), not
    // per sample, so the add costs nothing measurable.
    int32_t size() const { return mask + 1; }

    // `offset` is samples BEHIND the write head; the head decrements, so a
    // constant offset is exactly 1x forward playback.
    float read(bool right, int32_t offset) const {
        int32_t i = (write_ptr + offset) & mask;
        return (right ? r : l)[i];
    }
};

// Extra read heads (and, in zone R, an extra write head) on the FLUX tape.
class DustCloud {
public:
    static constexpr int kGrains = 8;

    void init(float sample_rate, uint32_t seed);
    void set_dust(float d);            // 0..1 amount
    void set_rot(float r);             // 0..1 character
    void set_delay_time(float s);      // FLUX delay time -> zone S grid

    bool  active() const { return _dust > 0.f; }
    float head_gain() const { return _head_gain; }   // echo head takeover
    float wear() const { return _wear; }             // freeze erosion coeff

    // Test/telemetry accessor: how many grains actually sounded on the last
    // process() call. Not read anywhere in the audio path itself — the density
    // claim ("more DUST = more grains") is otherwise only observable indirectly
    // through level, and the whole point of the Task 2 normalisation fix is
    // that level stays flat across the knob.
    int active_grains() const { return _active_grains; }

    // Advance one sample. Writes the stereo grain sum to gl/gr and returns the
    // tanh-bounded writeback sample (0 outside zone R).
    float process(const TapeTap& tape, float& gl, float& gr);

private:
    // 1/sqrt(k) for k = 0..kGrains, indexed by the number of grains actually
    // sounding this sample (spec §2, corrected 2026-07-19: normalise by what is
    // carried, not what is offered). A per-sample sqrt + divide costs ~30
    // cycles per part — this feature's whole CPU budget was measured without
    // one — so this table trades that for one array read. Index 0 is never
    // used (the caller clamps the index to >= 1) and holds 1.f as a harmless
    // placeholder.
    static constexpr float kInvSqrt[kGrains + 1] = {
        1.f, 1.f, 0.70710678f, 0.57735027f, 0.5f,
        0.44721360f, 0.40824829f, 0.37796447f, 0.35355339f
    };
    // Performance-shaped layout (see bench/workloads_dust.cpp proc_dust_opt):
    // the tape channel pointer is resolved once at birth (no per-sample L/R
    // select), the read position is an absolute index stepped by `rd_step`
    // (no per-sample recompute from write_ptr), and the Hann window is a
    // table index `widx` stepped by `wstep` and folded by a sign flip at the
    // midpoint (no per-sample age/length division, no per-sample call
    // through hann_value_at()).
    struct Grain {
        bool  alive = false;
        const float* tape = nullptr;  // resolved L or R channel base pointer
        int32_t rd = 0;                // absolute read index into `tape`
        int32_t rd_step = -1;          // delta - 1 (forward-only here: -1)
        int32_t age = 0;
        int32_t len = 1;
        float   widx = 0.f;            // 0..191 into the Hann table
        float   wstep = 0.f;
        float   gl = 0.f, gr = 0.f;    // equal-power pan gains
    };

    void _remap();                       // recompute derived values
    void _schedule(const TapeTap& tape);
    void _spawn(const TapeTap& tape);

    Grain _g[kGrains];
    Rng   _rng;

    // Hann table pointer, resolved once in init() rather than once per sample
    // in process(): hann_curve() returns a function-local static, so calling
    // it from the per-sample entry point pays that init guard ~48x more often
    // than the block-hoisted form the feature's CPU budget was measured on
    // (bench/workloads_dust.cpp proc_dust_opt hoists it out of the block loop;
    // process() here has no block loop of its own, so the member is the
    // per-sample equivalent of that hoist).
    const float* _curve = nullptr;

    float _sr = 48000.f;
    float _dust = 0.f;
    float _rot = 0.f;
    float _delay_time = 0.5f;

    int   _zone = 0;             // 0 = S sync, 1 = F free, 2 = R rot
    float _birth_prob = 0.f;     // zone F/R: per-sample birth probability
    int32_t _grid_period = 1;    // zone S: samples between grid slots
    int32_t _grid_countdown = 1;
    float _fire_prob = 0.f;      // zone S: chance a grid slot fires
    int   _burst = 1;            // zone S: grains per firing slot
    float _jitter = 0.f;         // zone S: 0 = locked, 1 = fully random
    float _len_min = 0.025f, _len_max = 0.1f;
    int32_t _spray = 1;          // max offset a new grain may start at
    float _rev_prob = 0.f;
    float _wb_gain = 0.f;
    float _norm = 1.f;             // smoothed 1/sqrt(active grains), see kInvSqrt
    float _norm_coef = 1.f;        // one-pole coefficient, kNormSmoothS at _sr
    int   _active_grains = 0;      // telemetry: grains sounding last process()
    float _head_gain = 1.f;
    float _wear = 1.f;
};

} // namespace spky
