#include <doctest/doctest.h>
#include <cmath>
#include "center/center.h"
#include "mod/super_modulator.h"
#include "mod/lane.h"
#include "mod/divisions.h"
#include "parts/part.h"
using namespace spky;

// Small fixture: two banks + two parts + a Center, all deterministically seeded.
namespace {
struct Rig {
    Center c; SuperModulator a, b; Part pa, pb;
    void init(uint32_t cseed = 123u) {
        c.init(48000.f, cseed);
        a.init(48000.f, 1u); b.init(48000.f, 2u);
        pa.init(48000.f, 1u); pb.init(48000.f, 2u);
    }
    void ticks(int n) { for (int k = 0; k < n; ++k) c.update(a, b, pa, pb); }
};

// Advance center AND lanes so phases actually move (ticks() only runs the center).
static void run_synced(Rig& r, int nticks) {
    for (int k = 0; k < nticks; ++k) {
        r.c.update(r.a, r.b, r.pa, r.pb);
        for (int s = 0; s < Center::kCtrlInterval; ++s) { r.a.process(); r.b.process(); }
    }
}

static float wrap_err(float e) { return e - std::floor(e + 0.5f); }
} // namespace

TEST_CASE("center morph: equal-power law holds across the sweep") {
    Rig r; r.init();
    for (float m = 0.f; m <= 1.001f; m += 0.05f) {
        r.c.set_morph(m);
        r.ticks(600);                                  // settle the smoother
        float ga = r.c.gain_a(), gb = r.c.gain_b();
        CHECK(ga * ga + gb * gb == doctest::Approx(1.f).epsilon(0.005));
    }
}

TEST_CASE("center morph: 0 is full A, 1 is full B") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(1.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(0.f).epsilon(0.005));
    r.c.set_morph(1.f); r.ticks(2000);
    CHECK(r.c.gain_a() == doctest::Approx(0.f).epsilon(0.005));
    CHECK(r.c.gain_b() == doctest::Approx(1.f).epsilon(0.005));
}

TEST_CASE("center morph: smoothing — no click-sized step per control tick after a jump") {
    Rig r; r.init();
    r.c.set_morph(0.f); r.ticks(2000);
    r.c.set_morph(1.f);                                // hard jump
    float prev = r.c.gain_a();
    for (int k = 0; k < 2000; ++k) {
        r.ticks(1);
        float g = r.c.gain_a();
        CHECK(std::fabs(g - prev) < 0.05f);
        prev = g;
    }
}

TEST_CASE("center drift: drift 0 leaves the rate hook at unity, weather at 0") {
    Rig r; r.init(7u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    float base = r.a.base_hz();
    r.c.set_couple(0.f); r.c.set_drift(0.f);
    r.ticks(5000);
    CHECK(r.a.master_hz() == doctest::Approx(base));
    CHECK(r.c.weather()   == doctest::Approx(0.f));
}

TEST_CASE("center drift: weather stays in (-1,1) and mean-reverts around 0") {
    Rig r; r.init(999u);
    r.c.set_drift(1.f);
    double sum = 0; int n = 0;
    for (int k = 0; k < 40000; ++k) {
        r.ticks(1);
        CHECK(r.c.weather() >  -1.f);
        CHECK(r.c.weather() <   1.f);
        sum += r.c.weather(); ++n;
    }
    CHECK(std::fabs(sum / n) < 0.5);
}

TEST_CASE("center drift: deterministic per seed") {
    auto run = [](uint32_t seed) {
        Rig r; r.init(seed);
        r.c.set_drift(1.f);
        float last = 0.f;
        for (int k = 0; k < 3000; ++k) { r.ticks(1); last = r.c.weather(); }
        return last;
    };
    CHECK(run(42u) == run(42u));
    CHECK(run(42u) != run(43u));
}

TEST_CASE("center drift: at full drift the two banks' rates move apart (opposite polarity)") {
    Rig r; r.init(321u);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    r.c.set_drift(1.f);
    bool a_moved = false, b_moved = false;
    float ba = r.a.base_hz(), bb = r.b.base_hz();
    for (int k = 0; k < 6000; ++k) {
        r.ticks(1);
        if (std::fabs(r.a.master_hz() - ba) > 1e-3f) a_moved = true;
        if (std::fabs(r.b.master_hz() - bb) > 1e-3f) b_moved = true;
    }
    CHECK(a_moved);
    CHECK(b_moved);
}

namespace {
void run_coupled(Rig& r, int samples) {
    int ctrl = 0;
    for (int i = 0; i < samples; ++i) {
        if (ctrl == 0) { r.c.update(r.a, r.b, r.pa, r.pb); ctrl = Center::kCtrlInterval; }
        --ctrl;
        r.a.process(); r.b.process();
    }
}
} // namespace

TEST_CASE("center couple: couple 1 locks two free banks and converges their rates") {
    Rig r; r.init(3u);
    r.a.set_rate(0.5f); r.b.set_rate(0.52f);
    r.c.set_couple(1.f);
    run_coupled(r, 48000 * 12);
    CHECK(std::fabs(r.c.phase_err()) < 0.03f);
    CHECK(r.a.master_hz() == doctest::Approx(r.b.master_hz()).epsilon(0.03));
}

TEST_CASE("center couple: couple 1 locks two FREE banks that are several octaves apart") {
    // free_hz maps the rate knob exponentially over 0.02..30 Hz, so 0.3 vs 0.7
    // is already an ~18:1 frequency ratio. Convergence to the geometric mean
    // needs each bank to scale by ~4.3x — which the old +/-1 octave rate clamp
    // truncated, leaving the pair drifting forever. Regression for the "COUPLE
    // full CW, FREE banks still drift apart" report.
    Rig r; r.init(3u);
    r.a.set_rate(0.3f); r.b.set_rate(0.7f);
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_coupled(r, 48000 * 20);
    CHECK(std::fabs(r.c.phase_err()) < 0.05f);
    CHECK(r.a.master_hz() == doctest::Approx(r.b.master_hz()).epsilon(0.03));
}

TEST_CASE("center couple: couple 1 holds the LOOP locked while EVOLVE wanders a bank") {
    // Both SYNC to the same grid division, full couple, no drift, bank A runs EVOLVE
    // (GROW) hard. The hard lock keeps the RAW pitch phase (the loop clock) pinned
    // despite EVOLVE's _ev_rate wander; the audible EVOLVE colour is free to move
    // within the cycle (that's what MELO is for) — we lock the loop, not the wander.
    Rig r; r.init(3u);
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);       // identical grid division
    r.a.set_variation(0.9f);                      // EVOLVE (GROW) hard on A
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    float worst = 0.f;
    for (int blk = 0; blk < 180; ++blk) {         // 3 minutes
        run_coupled(r, 48000);
        float d = r.a.pitch_phase() - r.b.pitch_phase();   // RAW loop clock
        d -= std::floor(d + 0.5f);                // wrap to [-0.5, 0.5)
        if (std::fabs(d) > worst) worst = std::fabs(d);
    }
    CHECK(worst < 0.02f);
}

TEST_CASE("center couple: couple full HARD-locks the loop through MELO / melody changes") {
    // Reported symptom: "drifts, especially when the melody changes; MELO 0 on both
    // sides syncs." MELO is VARIATION (EVOLVE): it walks _ev_rate, which modulates the
    // RAW phase increment by up to +/-20% (lane.cpp: _phase_inc*(1+_ev_rate)), so the
    // loops slip; and a phrase regen zeroes _ev_phase, kicking any eff-based lock. The
    // hard lock must hold the RAW pitch phase (the loop/sequencer clock) regardless of
    // MELO. We sample at the END of each 1 s window (after that window's regen) so this
    // asserts a sustained lock, not a transient.
    Rig r; r.init(3u);
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);       // identical grid division (~1 Hz)
    r.a.set_step(true, 8); r.b.set_step(true, 8);
    r.a.set_variation(0.9f);                      // MELO hard on A (the default is 0.32)
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    float worst = 0.f;
    for (int win = 0; win < 180; ++win) {         // 3 minutes, a fresh phrase each window
        r.a.new_phrase();                         // the melody changes
        run_coupled(r, 48000);                    // ~1 s
        float d = r.a.pitch_phase() - r.b.pitch_phase();   // RAW loop clock, not phase_eff
        d -= std::floor(d + 0.5f);                // wrap to [-0.5, 0.5)
        if (std::fabs(d) > worst) worst = std::fabs(d);
    }
    CHECK(worst < 0.02f);                         // hard lock: loops stay put under MELO
}

TEST_CASE("center couple: couple 0 leaves both rate hooks at unity") {
    Rig r; r.init(3u);
    r.a.set_rate(0.4f); r.b.set_rate(0.7f);
    float ba = r.a.base_hz(), bb = r.b.base_hz();
    r.c.set_couple(0.f); r.c.set_drift(0.f);
    run_coupled(r, 48000);
    CHECK(r.a.master_hz() == doctest::Approx(ba));
    CHECK(r.b.master_hz() == doctest::Approx(bb));
}

TEST_CASE("center grid: pitch sits on the division and the transport phase") {
    Rig r; r.init();
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f); r.c.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f);            // 1/4 -> 2 Hz
    r.b.set_rate(13.f / 16.f);     // 1/8T -> 6 Hz
    r.c.set_couple(1.f); r.c.set_drift(1.f);
    run_synced(r, 5000);           // 10 s: converge
    // rate: master pinned to the division despite full COUPLE + DRIFT
    CHECK(r.a.master_hz() == doctest::Approx(2.f).epsilon(0.02));
    CHECK(r.b.master_hz() == doctest::Approx(6.f).epsilon(0.02));
    // phase: each bank tracks its own grid target
    double beats = r.c.transport().beats();
    float ta = (float)(beats * 1.0 - std::floor(beats * 1.0));
    CHECK(std::fabs(wrap_err(ta - r.a.pitch_phase())) < 0.03f);
}

TEST_CASE("center grid: COUPLE 1 freezes the texture wander exactly") {
    Rig r; r.init(99u);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.c.set_couple(1.f); r.c.set_drift(1.f);
    run_synced(r, 3000);           // let the weather walk build up
    CHECK(r.a.mod_scale() == doctest::Approx(r.a.pitch_scale()));   // wander factor == 1
    CHECK(r.b.mod_scale() == doctest::Approx(r.b.pitch_scale()));
}

TEST_CASE("center grid: COUPLE 0 lets DRIFT breathe the textures apart") {
    Rig r; r.init(99u);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.c.set_couple(0.f); r.c.set_drift(1.f);
    bool wandered = false;
    for (int k = 0; k < 3000 && !wandered; ++k) {
        run_synced(r, 1);
        if (std::fabs(r.a.mod_scale() / r.a.pitch_scale() - 1.f) > 0.01f) wandered = true;
    }
    CHECK(wandered);
    // and the melody stays pinned regardless
    CHECK(r.a.master_hz() == doctest::Approx(r.a.base_hz()).epsilon(0.05));
}

TEST_CASE("center spot: shape kick decays back within ~5 s (lane level)") {
    // Base shape 0.2 keeps base+kick inside the gentle sine->triangle->ramp
    // region, so the output difference tracks the (decaying) shape offset rather
    // than the discontinuous pulse edge near shape 0.85. The spec decay is
    // tau ~ 1.5 s, so at 5 s the offset is ~3.6% of the kick — assert a relative
    // fade, not an absolute floor (which would demand a much shorter tau).
    ModLane kicked; kicked.init(48000.f, 9u); kicked.set_rate_hz(2.f); kicked.set_shape(0.2f);
    ModLane clean;  clean.init(48000.f, 9u);  clean.set_rate_hz(2.f);  clean.set_shape(0.2f);
    kicked.kick(0.f, 0.35f);
    float early = 0.f;
    for (int i = 0; i < 4800; ++i) {                          // first 0.1 s: audible
        float d = std::fabs(kicked.process() - clean.process());
        if (d > early) early = d;
    }
    for (int i = 0; i < 48000 * 5; ++i) { kicked.process(); clean.process(); }   // wait 5 s
    float late = 0.f;
    for (int i = 0; i < 480; ++i) {
        float d = std::fabs(kicked.process() - clean.process());
        if (d > late) late = d;
    }
    CHECK(early > 0.01f);              // the lightning flashed
    CHECK(late < early * 0.15f);       // and faded to a small fraction within ~5 s
}

TEST_CASE("lane settle: accelerates the return of an open shape kick") {
    ModLane ref;  ref.init(48000.f, 3u);  ref.set_rate_hz(1.f);  ref.set_shape(0.5f);
    ModLane slow; slow.init(48000.f, 3u); slow.set_rate_hz(1.f); slow.set_shape(0.5f);
    ModLane fast; fast.init(48000.f, 3u); fast.set_rate_hz(1.f); fast.set_shape(0.5f);
    slow.kick(0.f, 0.3f);
    fast.kick(0.f, 0.3f);
    fast.settle();
    for (int i = 0; i < 24000; ++i) { ref.process(); slow.process(); fast.process(); }
    float dslow = std::fabs(slow.process() - ref.process());
    float dfast = std::fabs(fast.process() - ref.process());
    CHECK(dfast <= dslow);         // settle pulled the kick home faster
}

TEST_CASE("center settle: weather and drift glide to 0 within ~1.5 s") {
    Rig r; r.init(11u);
    r.c.set_drift(1.f);
    r.ticks(4000);
    // NOTE(Task 7 checkpoint fix): the OU weather walk is genuinely random
    // (seeded, deterministic, but not tunable per-seed); 0.05 was a near-coin
    // -flip bound for this seed (measured ~0.037 with the correct, spec-
    // matching kOuTau/kOuSigma) — 0.02 still confirms weather has visibly
    // moved off 0 before checking that SETTLE brings it back down.
    CHECK(std::fabs(r.c.weather()) > 0.02f);
    r.c.settle(r.a, r.b);
    r.ticks(1500);                 // ~3 s at control rate
    CHECK(std::fabs(r.c.weather()) < 0.03f);
    CHECK(r.c.drift() < 0.05f);
}

TEST_CASE("center free: below COUPLE 0.5 the tempo has zero influence") {
    // Identical rigs, wildly different BPM -> identical rate hooks.
    Rig r1; r1.init(5u); Rig r2; r2.init(5u);
    r1.c.set_tempo_bpm(120.f); r2.c.set_tempo_bpm(77.f);
    for (Rig* r : {&r1, &r2}) {
        r->a.set_rate(0.62f); r->b.set_rate(0.44f);
        r->c.set_couple(0.4f); r->c.set_drift(0.f);
    }
    for (int k = 0; k < 4000; ++k) {
        run_synced(r1, 1); run_synced(r2, 1);
        CHECK(r1.a.pitch_scale() == doctest::Approx(r2.a.pitch_scale()).epsilon(1e-6));
        CHECK(r1.b.pitch_scale() == doctest::Approx(r2.b.pitch_scale()).epsilon(1e-6));
    }
}

TEST_CASE("center free: full COUPLE lands the pair on the ladder and the downbeat") {
    Rig r; r.init(11u);
    r.c.set_tempo_bpm(120.f);
    r.a.set_rate(0.60f); r.b.set_rate(0.52f);   // free Hz, off-grid geometric mean
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_synced(r, 15000);                        // 30 s to converge
    const float geo = std::sqrt(r.a.base_hz() * r.b.base_hz());
    const float grid = division_hz(nearest_division(geo, 120.f), 120.f);
    CHECK(r.a.master_hz() == doctest::Approx(grid).epsilon(0.03));
    CHECK(r.b.master_hz() == doctest::Approx(grid).epsilon(0.03));
    // pairwise phase lock still holds
    CHECK(std::fabs(r.c.phase_err()) < 0.03f);
    // and the pair sits on the transport grid phase
    const float cpb = kDivisions[nearest_division(geo, 120.f)].cpb;
    const double t = r.c.transport().beats() * (double)cpb;
    const float tgt = (float)(t - std::floor(t));
    CHECK(std::fabs(wrap_err(tgt - r.a.pitch_phase())) < 0.05f);
}

TEST_CASE("center grid: live STEPS turn is tempo-smooth — the servo target rebases") {
    // Rack report 2026-07-17: turning STEPS live briefly sped the loop up or
    // slowed it down (direction-dependent) until the pattern had run through.
    // set_step() preserves the local step position (spec: seamless turn), but
    // the grid target — total transport steps mod the NEW count — generally
    // lands elsewhere, so the hard servo dragged the rate by up to kLockCap
    // (±35%) until the phase reconverged. The rebase pins the servo target to
    // the lane's phase at the moment of the turn: position kept, tempo still.
    Rig r; r.init();
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f); r.c.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);          // same rung: 1/4 -> 2 Hz
    r.a.set_step(true, 8); r.b.set_step(true, 8);
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_synced(r, 5000);                             // 10 s: lock onto the grid
    // Advance to a moment where the 12-step grid target disagrees strongly
    // with the preserved step position — the worst case a live turn can hit.
    int guard = 0;
    for (; guard < 20000; ++guard) {
        run_synced(r, 1);
        const double t = r.c.transport().beats() * (8.0 / 12.0);
        const float tgt = (float)(t - std::floor(t));
        if (std::fabs(wrap_err(tgt - r.a.pitch_phase())) > 0.3f) break;
    }
    REQUIRE(guard < 20000);
    r.a.set_step(true, 12);                          // the live turn
    float worst = 0.f;
    for (int k = 0; k < 3000; ++k) {                 // the next ~6 s
        run_synced(r, 1);
        float d = std::fabs(r.a.pitch_scale() - 1.f);
        if (d > worst) worst = d;
    }
    CHECK(worst < 0.02f);                            // no tempo drag, ever
}

TEST_CASE("center grid: RST resyncs the loops onto the bar start, drag-free") {
    // The rebase (above) lets a live STEPS turn leave the loop free-running
    // against the bar (offset != 0). RST is the deliberate way back: it zeroes
    // the downbeat, drops the grid offsets, and the host snaps the lane phases
    // to 0 — everything restarts together at the bar start, with no servo drag.
    Rig r; r.init();
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f); r.c.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);
    r.a.set_step(true, 8); r.b.set_step(true, 8);
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_synced(r, 5000);
    int guard = 0;                                   // force a big offset first
    for (; guard < 20000; ++guard) {
        run_synced(r, 1);
        const double t = r.c.transport().beats() * (8.0 / 12.0);
        const float tgt = (float)(t - std::floor(t));
        if (std::fabs(wrap_err(tgt - r.a.pitch_phase())) > 0.3f) break;
    }
    REQUIRE(guard < 20000);
    r.a.set_step(true, 12);                          // live turn -> offset rebased
    run_synced(r, 1000);
    // the resync gesture
    r.c.reset_transport();
    r.a.reset_phases(); r.b.reset_phases();
    float worst = 0.f;
    for (int k = 0; k < 2500; ++k) {                 // ~5 s after RST
        run_synced(r, 1);
        float d = std::fabs(r.a.pitch_scale() - 1.f);
        if (d > worst) worst = d;
    }
    // Snap, not drag: the old path dragged at kLockCap (0.35). What remains is
    // a one-shot settle blip of kKHard*sin(2*pi*rate*tick) ~ 0.034 — update()
    // measures the lane one control tick behind the freshly zeroed transport,
    // and the servo walks that skew into its equilibrium once.
    CHECK(worst < 0.05f);
    // and the loop tracks the un-offset grid target again (12 steps: cpb x 8/12)
    const double t = r.c.transport().beats() * (8.0 / 12.0);
    const float tgt = (float)(t - std::floor(t));
    CHECK(std::fabs(wrap_err(tgt - r.a.pitch_phase())) < 0.03f);
    const double tb = r.c.transport().beats();       // B: 8 steps, factor 1
    const float tgtb = (float)(tb - std::floor(tb));
    CHECK(std::fabs(wrap_err(tgtb - r.b.pitch_phase())) < 0.03f);
}

TEST_CASE("center grid: step-clock — a 16-step bank locks at half the division rate") {
    // Step-clock (spec 2026-07-17): with S steps the pitch cycle spans S/8
    // divisions, so the grid servo must target beats*cpb*(8/S), not beats*cpb.
    // The unscaled target drags the lane back to cycle==division and re-imposes
    // the retired pattern-clock feel (Rack report 2026-07-17: STEPS changed the
    // melody tempo again and 12-vs-8 decks never met the same step grid).
    Rig r; r.init();
    r.a.set_tempo_bpm(120.f); r.b.set_tempo_bpm(120.f); r.c.set_tempo_bpm(120.f);
    r.a.set_synced(true); r.b.set_synced(true); r.c.set_sync(true);
    r.a.set_rate(0.5f); r.b.set_rate(0.5f);          // same rung: 1/4 -> 2 Hz
    r.a.set_step(true, 8); r.b.set_step(true, 16);   // same step tempo, B twice as long
    r.c.set_couple(1.f); r.c.set_drift(0.f);
    run_synced(r, 5000);                             // 10 s: converge
    // rate: the servo leaves both banks on their step-clock rate (pitch_scale ~1)
    CHECK(r.a.master_hz() == doctest::Approx(2.f).epsilon(0.02));
    CHECK(r.b.master_hz() == doctest::Approx(2.f).epsilon(0.02));
    // phase: each bank tracks its own step-scaled grid target
    const double beats = r.c.transport().beats();
    const float ta = (float)(beats - std::floor(beats));            // cpb 1 x 8/8
    const double tbd = beats * 0.5;                                 // cpb 1 x 8/16
    const float tb = (float)(tbd - std::floor(tbd));
    CHECK(std::fabs(wrap_err(ta - r.a.pitch_phase())) < 0.03f);
    CHECK(std::fabs(wrap_err(tb - r.b.pitch_phase())) < 0.03f);
}

// --- STEP-Einstiegs-Snap (spec 2026-07-23 sampler-performance-fixes) -------
//
// Die Phase wird AUF das Servo-Ziel gesetzt statt umgekehrt. Der Servofehler
// ist damit ab dem ersten Sample 0 -- das ist die Zusage gegenueber RST, das
// den Downbeat nullt und beide Decks mitreisst.
TEST_CASE("center: the FLOW->STEP snap lands the deck on the running grid") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);                       // Transport laeuft, Phasen wandern

    // Ein Offset, wie ihn der Free-Run hinterlaesst.
    r.pa.set_step(true, 8);                  // erste Beobachtung: kein Snap
    r.pa.set_step(false, 8);
    run_synced(r, 10);

    r.pa.set_step(true, 8);                  // DAS ist die Flanke
    r.c.update(r.a, r.b, r.pa, r.pb);

    // Ziel exakt so gerechnet, wie _grid_servo es tut -- mit genulltem Offset.
    const float cpb = kDivisions[r.a.division()].cpb * r.a.clock_scale();
    const double t  = r.c.transport().beats() * static_cast<double>(cpb);
    const float tgt = static_cast<float>(t - std::floor(t));
    CHECK(r.a.pitch_phase() == doctest::Approx(tgt).epsilon(1e-4));
}

// Kein Zerren: nach dem Snap darf der Servo das Deck nicht mehr ziehen. Ohne
// diese Zusicherung ginge auch ein naives "nur _grid_off nullen" durch -- und
// genau das zerrt (Rack-Bericht 2026-07-17).
TEST_CASE("center: the snap leaves no servo error behind") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);
    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    run_synced(r, 10);

    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    const float cpb = kDivisions[r.a.division()].cpb * r.a.clock_scale();
    const double t  = r.c.transport().beats() * static_cast<double>(cpb);
    const float err = wrap_err(static_cast<float>(t - std::floor(t)) - r.a.pitch_phase());
    CHECK(std::fabs(err) < 1e-4f);
}

// Das andere Deck und der Transport bleiben unangetastet -- der ganze Grund,
// warum das nicht reset_transport ruft. "Unangetastet" kann fuer den
// Transport nicht "unveraendert" heissen: tick() laeuft in Center::update
// bedingungslos zuerst und addiert pro Aufruf bpm/(60*cr) (transport.h:25).
// Die eigentliche Zusage ist, dass der Snap den Downbeat NICHT nullt, wie
// reset_transport() es taete -- der Snap tickt also normal weiter. Das wird
// gemessen, indem zuerst ein einfacher update()-Aufruf als Eichmass dient:
// sein Beat-Delta ist genau das, was jeder normale Tick bewirkt. Der
// Snap-Tick muss um dasselbe Delta vorruecken, nicht um 0 (das waere ein
// Reset) und nicht um irgendetwas anderes.
TEST_CASE("center: the snap touches neither the transport nor the other deck") {
    Rig r; r.init();
    r.c.set_sync(true);
    run_synced(r, 40);

    const float b_before = r.b.pitch_phase();

    // Eichmass: ein normaler update()-Tick ohne Snap.
    const double beats_before_probe = r.c.transport().beats();
    r.c.update(r.a, r.b, r.pa, r.pb);
    const double plain_delta = r.c.transport().beats() - beats_before_probe;

    const double beats_before_snap = r.c.transport().beats();
    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);
    const double snap_delta = r.c.transport().beats() - beats_before_snap;

    CHECK(r.b.pitch_phase() == doctest::Approx(b_before).epsilon(1e-6));
    CHECK(snap_delta == doctest::Approx(plain_delta).epsilon(1e-9));
}

// Freie Welt: ohne Transport ist das andere Deck die Referenz.
TEST_CASE("center: without SYNC the snap lands on the other deck's phase") {
    Rig r; r.init();
    r.c.set_sync(false);
    // Rig seeds both banks at the same rate norm (0.5), so left alone A and B
    // run bit-identically and never diverge -- the precondition below could
    // never hold. Give them clearly different free rates (COUPLE defaults to
    // 0 in Center::init, so nothing pulls them back together) so the phases
    // genuinely separate before the switch.
    r.a.set_rate(0.2f); r.b.set_rate(0.8f);
    run_synced(r, 40);
    REQUIRE(r.a.pitch_phase() != doctest::Approx(r.b.pitch_phase()).epsilon(1e-3));

    const float b_phase = r.b.pitch_phase();
    r.pa.set_step(true, 8);
    r.pa.set_step(false, 8);
    r.pa.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    CHECK(r.a.pitch_phase() == doctest::Approx(b_phase).epsilon(1e-4));
}

// Schalten beide im selben Tick, bleibt A stehen und B schnappt auf A. Ohne
// diese Regel schnappten beide auf die jeweils andere Vorher-Phase und
// tauschten sie nur.
TEST_CASE("center: when both decks switch in one tick, A is the reference") {
    Rig r; r.init();
    r.c.set_sync(false);
    // Same fix as above: without diverging rates A and B are bit-identical
    // and this precondition can never hold.
    r.a.set_rate(0.2f); r.b.set_rate(0.8f);
    run_synced(r, 40);
    const float a_before = r.a.pitch_phase();
    REQUIRE(r.b.pitch_phase() != doctest::Approx(a_before).epsilon(1e-3));

    r.pa.set_step(true, 8); r.pb.set_step(true, 8);
    r.pa.set_step(false, 8); r.pb.set_step(false, 8);
    r.pa.set_step(true, 8); r.pb.set_step(true, 8);
    r.c.update(r.a, r.b, r.pa, r.pb);

    CHECK(r.a.pitch_phase() == doctest::Approx(a_before).epsilon(1e-4));
    CHECK(r.b.pitch_phase() == doctest::Approx(a_before).epsilon(1e-4));
}

