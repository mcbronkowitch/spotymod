#include "workload.h"
#include <cmath>

namespace bench {
namespace {

// --- baseline: the fixed cost of the harness itself -------------------------
void setup_empty() {}
float proc_empty()
{
    // Nothing but the call and the loop the runner wraps around it. Every
    // other row is read as "cost above this".
    return 0.f;
}

// --- calibration: a load whose cycle cost is knowable by hand ---------------
// 96 samples x one sinf each. Not a musical workload; it exists so a human can
// tell "the counter works" from "the counter reads zero".
float g_phase = 0.f;
void setup_sinf() { g_phase = 0.f; }
float proc_sinf()
{
    float acc = 0.f;
    for (size_t i = 0; i < kBlock; ++i) {
        g_phase += 0.01f;
        acc += sinf(g_phase);
    }
    return acc;
}

} // namespace

const Workload kCoreWorkloads[] = {
    { "system", "empty_callback",  setup_empty, proc_empty },
    { "system", "sinf_x96",        setup_sinf,  proc_sinf  },
};
const int kCoreCount = sizeof(kCoreWorkloads) / sizeof(kCoreWorkloads[0]);

} // namespace bench
