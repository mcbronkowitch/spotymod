#include <doctest/doctest.h>
#include <cmath>
#include "mod/lane.h"
using namespace spky;

// Per-sample-vs-tick equivalence harness (spec 2026-07-19 mod-plane-control-
// rate, "Testing 1"). ref is driven by 96x process(), dut by one tick();
// both start from identical seed + config, so their private RNG streams are
// the same stream. Any skipped boundary or reordered draw desyncs the
// streams and explodes the target comparison within a few cycles.
namespace {
constexpr float kSr   = 48000.f;
constexpr int   kTick = ModLane::kTickInterval;

struct TickPair {
    ModLane ref, dut;
    float ref_out = 0.f, dut_out = 0.f;
    int   ref_fires = 0;
    bool  dut_fired = false;

    void boot(uint32_t seed, void (*cfg)(ModLane&)) {
        ref.init(kSr, seed); cfg(ref);
        dut.init(kSr, seed); cfg(dut);
    }
    void advance_one_tick() {
        ref_fires = 0;
        for (int i = 0; i < kTick; ++i) {
            ref_out = ref.process();
            if (ref.fired()) ++ref_fires;
        }
        dut_out = dut.tick();
        dut_fired = dut.fired();
    }
};
} // namespace

TEST_CASE("tick: STEP S&H targets and fires match the per-sample path exactly") {
    // shape 1.0 returns the S&H operand EXACTLY (entropy-sequencer fix), so
    // the boundary target is phase-independent: bit-equal across both paths.
    TickPair tp;
    tp.boot(42u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(2.3f);   // boundary every ~2609 smp
    });
    // One caveat: the two paths accumulate phase differently (96 rounded
    // adds vs one fused product), so a boundary landing within float-eps of
    // a tick edge can be detected one tick apart. That skew self-corrects on
    // the next tick and skips no RNG draw; the guard below tolerates exactly
    // that -- a real RNG desync would never re-converge and still fails.
    // Each straddle shows as TWO adjacent parity mismatches (early window +
    // missing next window), hence the doubled skew_events budget.
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
        CHECK(tp.dut_out == tp.ref_out);            // smooth 0 = passthrough
    }
    CHECK(skew_events <= 4);   // isolated float coincidences, never systematic
}

TEST_CASE("tick: GROW mutation dice stay on the same RNG stream") {
    // variation > 0 draws dice + walk deltas per boundary/wrap. 300 ticks
    // (~7 cycles) of exact target equality proves no draw was skipped.
    TickPair tp;
    tp.boot(7u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(3.7f);
        l.set_variation(0.7f);
    });
    // Same tick-edge skew guard as the S&H case: seed 7 / 3.7 Hz hits one
    // straddle (~tick 250). The draw is delayed one tick, never skipped;
    // exact equality must resume immediately after.
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: RENEW walk regen stays on the same RNG stream") {
    TickPair tp;
    tp.boot(11u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(3.1f);
        l.set_variation(-0.8f);
    });
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: FLOW output tracks the per-sample path") {
    // Continuous FLOW: same end phase modulo float accumulation -- the tick
    // path adds one fused product where the reference adds 96 rounded
    // increments. Loose epsilon, wrap-fire parity exact.
    TickPair tp;
    tp.boot(3u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.3f); l.set_smooth(0.f);
        l.set_rate_hz(1.9f);
    });
    for (int t = 0; t < 500; ++t) {
        tp.advance_one_tick();
        CHECK((tp.ref_fires > 0) == tp.dut_fired);
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.01));
    }
}

TEST_CASE("tick: SMOOTH slew matches outside a post-boundary blackout") {
    // The tick coefficient is the exact 96-sample compound of the per-sample
    // coefficient, so held segments converge identically. A boundary lands
    // mid-interval for the reference but takes effect at the tick edge for
    // the dut -- allow a 2-tick blackout after each fire, then require the
    // paths to agree again.
    TickPair tp;
    tp.boot(9u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.5f);
        l.set_step(true, 8); l.set_rate_hz(2.3f);
    });
    int blackout = 2;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if (tp.ref_fires > 0 || tp.dut_fired) { blackout = 2; continue; }
        if (blackout > 0) { --blackout; continue; }
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.02));
    }
}

TEST_CASE("tick: multiple boundaries inside one interval are replayed in order") {
    // 500 Hz at 8 steps = one boundary every 12 samples = 8 per tick. With
    // GROW dice active, a single skipped or reordered boundary desyncs the
    // RNG stream and the exact target comparison fails within a few ticks.
    TickPair tp;
    tp.boot(21u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(500.f);
        l.set_variation(0.6f);
    });
    // A boundary landing within float-eps of a tick edge shifts that one
    // boundary into the neighbouring window: the straddle tick compares
    // different "last boundary" targets, then equality resumes. Tolerate
    // isolated straddle ticks, never sustained divergence -- a skipped or
    // reordered boundary desyncs the RNG stream permanently and blows the
    // mismatch budget.
    int mismatch = 0;
    for (int t = 0; t < 200; ++t) {
        tp.advance_one_tick();
        if (tp.dut.target() != tp.ref.target()) { ++mismatch; continue; }
    }
    CHECK(mismatch <= 2);                          // isolated straddles only
    CHECK(tp.dut.target() == tp.ref.target());     // re-converged at the end
}

TEST_CASE("tick: wrap events land before the new cycle's step 0") {
    // variation -1 makes the RENEW walk-regen dice certain (v^2 = 1), so the
    // whole _seq walk regenerates at EVERY wrap. Step 0's target right after
    // the seam must sample the NEW walk -- if tick() ran the step-0 boundary
    // before _wrap_events(), it would sample the old walk and diverge from
    // the per-sample reference immediately.
    TickPair tp;
    tp.boot(33u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(1.f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(4.3f);
        l.set_variation(-1.f);
    });
    int skew = 0, skew_events = 0;
    for (int t = 0; t < 300; ++t) {
        tp.advance_one_tick();
        if ((tp.ref_fires > 0) != tp.dut_fired) { skew = 1; ++skew_events; continue; }
        if (skew > 0) { --skew; continue; }
        CHECK(tp.dut.target() == tp.ref.target());
    }
    CHECK(skew_events <= 4);
}

TEST_CASE("tick: SPOT kick equivalence at tick granularity") {
    // Kick applied to both paths at a tick edge (the only place Center can
    // apply it in production -- SPOT runs on the control tick).
    TickPair tp;
    tp.boot(5u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.4f); l.set_smooth(0.f);
        l.set_step(true, 8); l.set_rate_hz(2.9f);
    });
    for (int t = 0; t < 50; ++t) tp.advance_one_tick();
    tp.ref.kick(0.3f, 0.2f);
    tp.dut.kick(0.3f, 0.2f);
    int blackout = 0;
    for (int t = 0; t < 400; ++t) {
        tp.advance_one_tick();
        if (tp.ref_fires > 0 || tp.dut_fired) { blackout = 2; continue; }
        if (blackout > 0) { --blackout; continue; }
        // shape 0.4 is phase-dependent: boundary targets differ by the
        // detection-overshoot phase (< 1 sample) -- loose but real bound.
        CHECK(tp.dut_out == doctest::Approx(tp.ref_out).epsilon(0.05));
    }
}

TEST_CASE("tick: SETTLE glides the audible phase the same way") {
    // Build up EVOLVE walks first (variation > 0), then settle both paths and
    // compare the audible phase while the glide runs (tau 0.3 s, ctr 1 s).
    TickPair tp;
    tp.boot(13u, [](ModLane& l) {
        l.set_range(1.f); l.set_shape(0.5f); l.set_smooth(0.f);
        l.set_rate_hz(2.f);
        l.set_variation(0.8f);
    });
    for (int t = 0; t < 1500; ++t) tp.advance_one_tick();   // ~3 s of walk
    tp.ref.settle();
    tp.dut.settle();
    for (int t = 0; t < 600; ++t) {                          // ~1.2 s glide
        tp.advance_one_tick();
        // circular distance: phases straddling the 1.0 wrap must not read
        // as a full-cycle disagreement (0.999 vs 0.001 is 0.002 apart)
        float d = std::fabs(tp.dut.phase_eff() - tp.ref.phase_eff());
        if (d > 0.5f) d = 1.f - d;
        CHECK(d < 0.01f);
    }
}
