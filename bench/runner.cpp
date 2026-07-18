#include "workload.h"
#include "cycles.h"
#include <cstring>

namespace bench {

namespace {
// FNV-1a over the raw float bits. Exact, order-sensitive, and cheap enough
// to sit inside the measured loop without distorting it (a handful of cycles
// against a 960 000-cycle budget).
inline uint32_t fold(uint32_t h, float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return (h ^ bits) * 16777619u;
}
} // namespace

Result run_workload(const Workload& w)
{
    Result r;
    w.setup();

    // Warm-up: settle caches, envelopes, delay lines and any smoothing state
    // so the measured window sees steady-state cost, not attack transients.
    uint32_t h = 2166136261u;
    for (int i = 0; i < kWarmupBlocks; ++i) h = fold(h, w.process());

    uint64_t total = 0;                 // 1000 x up to 9.6M cycles overflows 32 bits
    for (int i = 0; i < kMeasBlocks; ++i) {
        const uint32_t t0 = cycles_now();
        const float v = w.process();
        const uint32_t dt = cycles_now() - t0;   // wraps correctly on unsigned
        h = fold(h, v);
        total += dt;
        if (dt > r.max_cyc) r.max_cyc = dt;
        if (dt > kBudgetCycles * 10u) {          // runaway: abort, do not hang
            r.timed_out = true;
            r.checksum = h;
            return r;
        }
    }

    r.avg_cyc  = static_cast<uint32_t>(total / kMeasBlocks);
    r.checksum = h;
    return r;
}

const Workload* find_workload(const char* name)
{
    const Workload* tables[] = { kCoreWorkloads, kVoiceWorkloads, kMemWorkloads, kModWorkloads };
    const int       counts[] = { kCoreCount,     kVoiceCount,     kMemCount,     kModCount     };
    for (int t = 0; t < 4; ++t)
        for (int i = 0; i < counts[t]; ++i)
            if (std::strcmp(tables[t][i].name, name) == 0) return &tables[t][i];
    return nullptr;
}

} // namespace bench
