#include "workload.h"
#include "mod/lane.h"
#include "mod/lane_id.h"
#include "mod/super_modulator.h"
#include "center/center.h"
#include "parts/part.h"

namespace bench {
namespace {

using namespace spky;

// --- one lane, FLOW, at each of shape_value()'s four segments ---------------
//
// shape_value() switches on floor(shape * 4), and ONLY segment 0 evaluates
// wave_sine. So the spread across these four rows is itself the measurement:
// if shape0 costs materially more than shape3, the libm sine is the reason,
// and the size of that gap is what the next task removes.
ModLane g_lane;

void setup_lane_flow(float shape)
{
    g_lane.init(kSampleRate, 5u);
    g_lane.set_rate_hz(2.f);
    g_lane.set_shape(shape);
    g_lane.set_density(1.f);      // every boundary fires; no frozen-hold shortcut
    g_lane.set_smooth(0.5f);
    g_lane.set_step(false, 8);    // FLOW: _compute_raw runs every sample
}

void setup_lane_flow_s00() { setup_lane_flow(0.f);  }   // sine -> triangle
void setup_lane_flow_s03() { setup_lane_flow(0.3f); }   // triangle -> ramp
void setup_lane_flow_s07() { setup_lane_flow(0.7f); }   // ramp -> pulse
void setup_lane_flow_s10() { setup_lane_flow(1.f);  }   // pure S&H

// STEP: does _compute_raw really run only on a fire? If so this row should
// come in far below the FLOW rows at the same shape.
void setup_lane_step()
{
    g_lane.init(kSampleRate, 5u);
    g_lane.set_rate_hz(2.f);
    g_lane.set_shape(0.f);        // same segment as setup_lane_flow_s00
    g_lane.set_density(1.f);
    g_lane.set_smooth(0.5f);
    g_lane.set_step(true, 8);
}

float proc_lane()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        acc += g_lane.process();
        // Fold the fire flag in: a lane whose phase never advances would
        // otherwise be indistinguishable from a working one by checksum.
        if (g_lane.fired()) acc += 1.f;
    }
    return acc;
}

// --- one whole SuperModulator (LANE_COUNT lanes) ----------------------------
// Compared against 5x lane_flow_shape00 this shows whether the bank adds cost
// above its lanes, or is just their sum.
SuperModulator g_sm;

void setup_super_mod()
{
    g_sm.init(kSampleRate, 1u);
    g_sm.set_rate(0.5f);
    g_sm.set_density(1.f);
    g_sm.set_shape(0.f);          // same segment as the single-lane rows
    g_sm.set_smooth(0.5f);
}

float proc_super_mod()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_sm.process();
        acc += g_sm.lane_output(LANE_PITCH);
    }
    return acc;
}

// --- the Center tick alone --------------------------------------------------
// Center::update runs once per kCtrlInterval (96) samples, i.e. exactly once
// per block. The modulators and parts here are hooks it writes through; they
// are deliberately NOT advanced, so this row prices the tick and nothing else.
SuperModulator g_c_a, g_c_b;
Center         g_center;
Part           g_c_pa, g_c_pb;

void setup_center_tick()
{
    g_c_a.init(kSampleRate, 1u);
    g_c_b.init(kSampleRate, 2u);
    g_c_pa.init(kSampleRate, 1u);
    g_c_pb.init(kSampleRate, 2u);
    g_center.init(kSampleRate, 11u);
    g_center.set_morph(0.5f);
    g_center.set_couple(0.7f);    // grid-gravity zone: the servo maths runs
    g_center.set_drift(0.7f);     // weather OU process active
}

float proc_center_tick()
{
    g_center.update(g_c_a, g_c_b, g_c_pa, g_c_pb);
    return g_center.morph() + g_center.weather();
}

} // namespace

const Workload kModWorkloads[] = {
    { "mod", "lane_flow_shape00", setup_lane_flow_s00, proc_lane      },
    { "mod", "lane_flow_shape03", setup_lane_flow_s03, proc_lane      },
    { "mod", "lane_flow_shape07", setup_lane_flow_s07, proc_lane      },
    { "mod", "lane_flow_shape10", setup_lane_flow_s10, proc_lane      },
    { "mod", "lane_step_shape00", setup_lane_step,     proc_lane      },
    { "mod", "super_mod_5lanes",  setup_super_mod,     proc_super_mod },
    { "mod", "center_tick",       setup_center_tick,   proc_center_tick },
};
const int kModCount = sizeof(kModWorkloads) / sizeof(kModWorkloads[0]);

} // namespace bench
