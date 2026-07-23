#include "center/center.h"
#include <cmath>

using namespace spky;

namespace {
constexpr float kQuarter = TWO_PI * 0.25f;   // pi/2, for the equal-power law

constexpr float kOuTau   = 45.f;             // weather time constant (s)
constexpr float kOuSigma = 0.10f;            // weather noise scale

// DRIFT taps (polarity/scale of the full walk — spec table). Index 0 = A, 1 = B.
constexpr float kRateTap[2]  = { 1.0f, -0.6f };   // x kRateOct octaves
constexpr float kShapeTap[2] = { 0.8f, -1.0f };   // x kShapeMax
constexpr float kTuneTap[2]  = { 0.5f, -0.9f };   // x kTuneCents
constexpr float kRateOct   = 0.5f;                // up to +/- 1/2 octave
constexpr float kShapeMax  = 0.15f;               // up to +/- 0.15 shape
constexpr float kTuneCents = 25.f;                // up to +/- 25 cents

constexpr float kK           = 0.15f;   // Kuramoto phase-pull gain (tune by ear)
// COUPLE convergence pulls a FREE bank toward the geometric mean of the two
// base rates (always <= the faster bank, so it can never run away to audio
// rate). The clamp is only a transient safety net, so it must be wide enough
// that full COUPLE can actually reach the mean across the musical rate range;
// the old +/-1 octave (0.5..2.0) truncated convergence and left banks that were
// more than a ~4:1 ratio apart perpetually drifting. +/-5 octaves covers the
// whole practical FREE span (0.02..30 Hz needs up to ~39x; this reaches ~32x).
constexpr float kRateClampLo = 0.03125f;   // 1/32  (-5 octaves)
constexpr float kRateClampHi = 32.0f;      //        (+5 octaves)

// COUPLE fully clockwise (knob == 1) is a HARD lock, not the soft Kuramoto nudge.
// Two things are essential and were both wrong before:
//  1. Lock the RAW pitch phase (the sawtooth that clocks the sequencer / loop), NOT
//     phase_eff. phase_eff adds the EVOLVE offset _ev_phase, which a melody change
//     (MELO != 0) zeroes at the phrase wrap — a step the servo then chased by shoving
//     the raw clock around, i.e. the "drifts when the melody changes" report. The raw
//     phase is continuous through a regen, so a melody change no longer disturbs it.
//  2. Push hard enough to overcome EVOLVE's rate wander. MELO drives _ev_rate, which
//     modulates the raw phase increment by up to +/-20% (lane.cpp: _phase_inc*(1+_ev_rate)).
//     The gentle kK=0.15 nudge can't outrun that, so the loops slip. At full COUPLE we
//     use kKHard with a per-tick slew cap: the cap keeps engage/large errors click-free
//     and the servo stable, while kKHard > the disturbance drives the residual to ~0.
constexpr float kFullCouple = 0.999f;   // knob effectively fully CW -> hard lock
constexpr float kKHard      = 2.0f;     // hard-lock phase-pull gain (>> the _ev_rate wander)
constexpr float kLockCap    = 0.35f;    // max rate correction per tick (> _ev_rate's 0.2)
}

void Center::init(float sample_rate, uint32_t seed) {
    _sr = sample_rate;
    _cr = sample_rate / static_cast<float>(kCtrlInterval);
    _transport.init(_cr);

    _morph_target = 0.5f; _morph = 0.5f;
    _morph_smooth.init(_cr, 0.03f);
    _morph_smooth.reset(0.5f);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    _couple = 0.f; _phase_err = 0.f;

    _drift_target = 0.f; _drift = 0.f;
    _drift_smooth.init(_cr, 0.3f);
    _drift_smooth.reset(0.f);
    _ou = 0.f; _weather = 0.f;
    _weather_rng.seed(seed);

    _spot_rng.seed(seed ^ 0x51207AB7u);
    _settle_ctr = 0;
    _settle_coef = std::exp(-1.f / (0.3f * _cr));
}

void Center::update(SuperModulator& a, SuperModulator& b, Part& pa, Part& pb) {
    _transport.tick();
    _rebase_grid(a, 0);
    _rebase_grid(b, 1);

    // Dieser Block steht rein aus Lesbarkeitsgruenden NACH den beiden
    // _rebase_grid-Aufrufen -- die Korrektheit haengt nicht an dieser
    // Reihenfolge. Was den gerade genullten Offset tatsaechlich schuetzt,
    // ist dass _snap_phase das neue clock_scale selbst in _grid_cs[i]
    // vermerkt (siehe unten, _grid_cs[i] = m.clock_scale()): _rebase_grid
    // vergleicht exakt gegen _grid_cs[i] und kehrt sofort zurueck, wenn
    // beide uebereinstimmen -- das gilt fuer diesen Tick ebenso wie fuer
    // den naechsten. Liefe der Snap vor dem Rebase, faende der Rebase sein
    // Deck also bereits mit dem neuen clock_scale verbucht vor und liesse
    // es unangetastet; die beiden Aufrufe kommutieren.
    //
    // Bank A ist die Phasenreferenz des Paars (siehe die Grid-Gravity unten).
    // In der freien Welt (SYNC aus) heisst das: schaltet nur ein Deck, snappt
    // es aufs andere; schalten beide im selben Tick, hat A keine aeussere
    // Referenz mehr, an der es sich ausrichten koennte -- also bleibt A
    // stehen, und B landet auf A. "A bleibt stehen" heisst aber NICHT "A wird
    // uebersprungen": A bekommt trotzdem seinen eigenen _snap_phase-Aufruf,
    // nur mit sich selbst als Referenz (tgt = other.pitch_phase() mit
    // other == a == As eigene, noch nicht veraenderte Phase). Der Phasen-Snap
    // ist dadurch ein No-Op (Ziel == Ist), aber Cursor-Ausrichtung und
    // Offset-Nullung laufen fuer A genauso wie fuer jedes andere snappende
    // Deck -- sonst bekaeme A beim FLOW->STEP-Einstieg einen veralteten
    // Slice-Cursor und ein ungenulltes _grid_off, obwohl es gerade denselben
    // Einstieg vollzieht wie B. (snap_pitch_phase raeumt nebenbei auch As
    // Onset-Gap-Ring leer -- gewollt: A steigt genau wie B gerade in STEP
    // ein, und das Aufraeumen ist, was beide Decks dabei bekommen.) Das ist
    // eine explizite Fallunterscheidung, keine Konsequenz der Aufrufreihenfolge:
    // wuerde A trotzdem zuerst konsumiert und mit B als Referenz snappen,
    // saehe B nur As bereits gesnappte (= Bs alte) Phase, und beide laufen am
    // Ende auf Bs alter Phase zusammen -- die beiden tauschen dann bloss ihre
    // Vorher-Phasen, statt dass A Referenz bleibt. Beide Flags werden in
    // jedem Fall konsumiert (take_step_snap loescht sie). Im GRID-Modus
    // (SYNC an) snappt jedes Deck unabhaengig aufs Transport-Raster, da gibt
    // es diese Wechselwirkung nicht.
    const bool snap_a = pa.take_step_snap();
    const bool snap_b = pb.take_step_snap();
    if (_sync) {
        if (snap_a) _snap_phase(a, pa, 0, b);
        if (snap_b) _snap_phase(b, pb, 1, a);
    } else if (snap_a && snap_b) {
        _snap_phase(a, pa, 0, a);   // A bleibt Referenz: eigene Phase als Ziel
        _snap_phase(b, pb, 1, a);   // B landet auf A
    } else if (snap_a) {
        _snap_phase(a, pa, 0, b);
    } else if (snap_b) {
        _snap_phase(b, pb, 1, a);
    }

    // --- MORPH (equal-power, smoothed at control rate) ---
    _morph = _morph_smooth.process(_morph_target);
    _g_a = std::cos(_morph * kQuarter);
    _g_b = std::sin(_morph * kQuarter);

    // --- DRIFT amount (smoothed) + weather step ---
    _drift = _drift_smooth.process(_drift_target);
    _step_weather();
    const float w = _weather * _drift;          // exactly 0 while drift is 0

    const float rate_drift_a = std::pow(2.f, kRateOct * kRateTap[0] * w);
    const float rate_drift_b = std::pow(2.f, kRateOct * kRateTap[1] * w);
    a.set_shape_offset(kShapeMax * kShapeTap[0] * w);
    b.set_shape_offset(kShapeMax * kShapeTap[1] * w);
    pa.set_detune_cents(kTuneCents * kTuneTap[0] * w);
    pb.set_detune_cents(kTuneCents * kTuneTap[1] * w);

    // --- COUPLE (Kuramoto PLL: convergence toward the geometric mean + phase pull) ---
    // Lock the RAW pitch phase — the sawtooth that clocks the loop/sequencer. NOT
    // phase_eff: that adds the per-bank EVOLVE offset _ev_phase, which a melody change
    // (MELO) zeroes at the phrase wrap. Locking phase_eff made every melody change
    // shove the raw clock to compensate — the "drifts when the melody changes" report.
    // The raw phase is continuous through a regen, so melody changes no longer disturb it.
    float dphi = a.pitch_phase() - b.pitch_phase();
    dphi -= std::floor(dphi + 0.5f);            // wrap to [-0.5, 0.5)
    _phase_err = dphi;

    if (_sync) {
        // GRID WORLD: melody/steps live on the transport; COUPLE only sets how
        // tightly the four texture lanes follow. texture = 0 at full COUPLE
        // -> the DRIFT rate wander is fully suppressed and the mod lanes hold
        // their exact ratios (lockstep); smaller COUPLE lets it through.
        const float pitch_a = 1.f + _grid_servo(a, _grid_off[0]);
        const float pitch_b = 1.f + _grid_servo(b, _grid_off[1]);
        const float texture = 1.f - _couple;
        const float mod_a = pitch_a * std::pow(rate_drift_a, texture);
        const float mod_b = pitch_b * std::pow(rate_drift_b, texture);
        a.set_rate_scale(pitch_a, mod_a);
        b.set_rate_scale(pitch_b, mod_b);
    } else {
        // FREE WORLD: geometric-mean convergence + Kuramoto phase pull; at
        // full COUPLE this becomes a hard lock (Task 5 adds grid gravity).
        const float fa = a.base_hz(), fb = b.base_hz();

        // convergence: both banks slide toward the geometric mean.
        const float conv_e = _couple * 0.5f;
        float conv_a = std::pow(fb / fa, conv_e);
        float conv_b = std::pow(fa / fb, conv_e);

        // phase pull: opposite sign on the two banks. At full COUPLE this becomes a
        // HARD lock — a much stronger gain (kKHard) with a per-tick slew cap
        // (kLockCap): the cap keeps engage click-free and the loop stable, while
        // the strong gain outruns EVOLVE's raw-rate wander so the residual phase
        // error collapses to ~0. Below full COUPLE the gentle kK nudge acts as
        // before (an approximate lock that lets the banks breathe).
        const bool hard = _couple >= kFullCouple;
        const float s = std::sin(TWO_PI * dphi);
        float corr = _couple * (hard ? kKHard : kK) * s;
        if (hard) corr = clampf(corr, -kLockCap, kLockCap);
        float pull_a = 1.f - corr;
        float pull_b = 1.f + corr;

        // Grid gravity: above COUPLE 0.5 the pair is additionally pulled onto
        // the nearest ladder division (rate) and the transport grid (phase).
        // Below 0.5 g == 0 exactly — the organic pairwise character is
        // untouched and provably tempo-free. smoothstep avoids a corner at 0.5.
        float g = 0.f;
        if (_couple > 0.5f) {
            const float z = (_couple - 0.5f) * 2.f;
            g = z * z * (3.f - 2.f * z);
        }
        if (g > 0.f) {
            const float geo  = std::sqrt(fa * fb);
            const int   div  = nearest_division(geo, _transport.bpm());
            const float grid = division_hz(div, _transport.bpm());
            const float grid_mult = std::pow(grid / geo, g);   // common-mode rate pull
            conv_a *= grid_mult;
            conv_b *= grid_mult;
            // Common-mode phase gravity. Bank A is the pair's phase reference:
            // at this COUPLE level the pairwise pull already holds A and B
            // together, so steering A steers the pair.
            // Bank A is the phase reference, so its step-clock factor scales
            // the grid target (spec 2026-07-17 step-clock).
            const double t = _transport.beats()
                             * static_cast<double>(kDivisions[div].cpb * a.clock_scale())
                             + static_cast<double>(_grid_off[0]);
            const float tgt = static_cast<float>(t - std::floor(t));
            float cme = tgt - a.pitch_phase();
            cme -= std::floor(cme + 0.5f);
            const float cm = g * (hard ? clampf(kKHard * std::sin(TWO_PI * cme), -kLockCap, kLockCap)
                                       : kK * std::sin(TWO_PI * cme));
            pull_a *= (1.f + cm);
            pull_b *= (1.f + cm);
        }

        const float mult_a = clampf(conv_a * pull_a, kRateClampLo, kRateClampHi);
        const float mult_b = clampf(conv_b * pull_b, kRateClampLo, kRateClampHi);

        // single rate hook = COUPLE x DRIFT rate tap, applied symmetrically to
        // pitch and mod lanes (Tasks 4-5 split these apart)
        a.set_rate_scale(mult_a * rate_drift_a, mult_a * rate_drift_a);
        b.set_rate_scale(mult_b * rate_drift_b, mult_b * rate_drift_b);
    }
}

// Per-tick rate correction that servos a synced bank's pitch phase onto its
// own grid target (transport beats x the bank's division). Uses the same
// sin-shaped hard-lock law as the free world's full-COUPLE lock (small-signal
// gain kKHard*2*pi ~ 12.6, cap kLockCap keeps engage click-free) — that is
// what outruns EVOLVE's +/-20% raw-rate wander, matching the 2026-07-15 lock
// quality the existing EVOLVE-wander test asserts.
// A live STEPS turn jumps the grid target (total transport steps mod the NEW
// count) while set_step() preserves the local step position — left alone, the
// hard servo drags the tempo by up to kLockCap until the phase reconverges
// (Rack report 2026-07-17: live STEPS turns briefly sped up / slowed down).
// Rebase the per-bank target offset onto the lane's current phase, so the
// servo error restarts at 0: position kept, tempo untouched. The offset
// persists — the loop free-runs against the bar — until RST resyncs it.
void Center::_rebase_grid(const SuperModulator& m, int i) {
    const float cs = m.clock_scale();
    if (cs == _grid_cs[i]) return;
    _grid_cs[i] = cs;
    const double t = _transport.beats()
                     * static_cast<double>(kDivisions[m.division()].cpb * cs);
    float off = m.pitch_phase() - static_cast<float>(t - std::floor(t));
    off -= std::floor(off);                         // keep in [0,1)
    _grid_off[i] = off;
}

// Siehe die Deklaration in center.h. Der Offset wird ZUERST genullt: das Ziel
// unten muss mit dem genullten Offset gerechnet werden, sonst landet der Snap
// um genau den alten Offset daneben. In der freien Welt liest den Offset
// niemand -- genullt wird er trotzdem: _grid_servo rechnet
// target = beats*cpb + off, ein genullter Offset ist deshalb die
// fehlerfreie Wahl (ein spaeter eingeschaltetes SYNC beginnt bei Fehler 0);
// ein rebase-artiger (nicht genullter) Offset waere die Variante, an der ein
// spaeter eingeschaltetes SYNC zoege. Genullt wird trotzdem, weil das zu
// reset_transport()'s "auf den Takt ausrichten"-Konvention passt (center.h:33).
void Center::_snap_phase(SuperModulator& m, Part& p, int i,
                          const SuperModulator& other) {
    _grid_off[i] = 0.f;
    _grid_cs[i]  = m.clock_scale();   // sonst rebased der naechste Tick sofort

    float tgt;
    if (_sync) {
        const float cpb = kDivisions[m.division()].cpb * m.clock_scale();
        const double t  = _transport.beats() * static_cast<double>(cpb);
        tgt = static_cast<float>(t - std::floor(t));
    } else {
        // Ohne Transport gibt es kein Raster: das andere Deck ist die einzige
        // sinnvolle Referenz. Roh auf roh -- die Kopplung und beide Servos
        // rechnen mit pitch_phase(), und die Schrittgrenzen der Lane haengen
        // an derselben Groesse.
        tgt = other.pitch_phase();
    }

    m.snap_pitch_phase(tgt);
    p.snap_sampler_cursor(ModLane::step_index(tgt, m.pitch_steps()));
}

float Center::_grid_servo(const SuperModulator& m, float off) const {
    // Step-clock (spec 2026-07-17): with S steps the pitch cycle spans S/8
    // divisions, so the grid target runs at cpb x 8/S. Without this scale the
    // servo drags the lane back to cycle==division and re-imposes the retired
    // pattern-clock feel whenever SYNC is on.
    const float cpb = kDivisions[m.division()].cpb * m.clock_scale();
    const double t = _transport.beats() * static_cast<double>(cpb)
                     + static_cast<double>(off);
    const float target = static_cast<float>(t - std::floor(t));
    float err = target - m.pitch_phase();
    err -= std::floor(err + 0.5f);                  // wrap to [-0.5, 0.5)
    return clampf(kKHard * std::sin(TWO_PI * err), -kLockCap, kLockCap);
}

void Center::_step_weather() {
    const float dt = static_cast<float>(kCtrlInterval) / _sr;   // control-tick period (s)
    if (_settle_ctr > 0) {
        --_settle_ctr;
        _ou *= _settle_coef;                    // panic: glide the walk to 0
    } else if (_drift > 0.f) {                   // no drift -> no weather system running
        // Ornstein-Uhlenbeck: mean-revert to 0, add scaled white noise. Noise
        // is scaled by _drift (not just gated by it > 0): the smoothed _drift
        // value asymptotically approaches but never exactly reaches 0, so a
        // bare boolean gate would let full-strength noise resume the instant
        // the SETTLE countdown (Task 7 checkpoint fix) expires, even while
        // _drift is still negligibly small — defeating SETTLE's purpose.
        _ou += (-_ou / kOuTau) * dt + kOuSigma * _drift * std::sqrt(dt) * _weather_rng.next_bipolar();
    }
    _weather = std::tanh(_ou);                  // softly bounded to (-1, 1)
}

void Center::spot(SuperModulator& a, SuperModulator& b) {
    a.spot(_spot_rng); b.spot(_spot_rng);
}

void Center::settle(SuperModulator& a, SuperModulator& b) {
    _drift_target = 0.f;
    _settle_ctr = static_cast<int>(_cr * 1.5f);
    a.settle(); b.settle();
}
