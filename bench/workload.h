#pragma once
#include <cstdint>
#include <cstddef>

namespace bench {

// Fixed measurement conditions (spec "Measurement conditions").
constexpr size_t   kBlock        = 96;        // samples per process() call
constexpr float    kSampleRate   = 48000.f;
constexpr uint32_t kBudgetCycles = 960000;    // 96/48000 s at 480 MHz
constexpr int      kWarmupBlocks = 100;
constexpr int      kMeasBlocks   = 1000;

// A workload is one table row. process() renders exactly kBlock samples and
// returns a value derived from what it produced -- the runner folds that into
// a checksum, which is what keeps -ffast-math from deleting the whole thing.
struct Workload {
    const char* family;
    const char* name;
    void  (*setup)();
    float (*process)();
};

struct Result {
    uint32_t avg_cyc   = 0;
    uint32_t max_cyc   = 0;
    uint32_t checksum  = 0;
    bool     timed_out = false;
};

Result run_workload(const Workload& w);

// The three family tables. Static, not self-registering: table order is
// execution order and must not depend on link order (plan deviation 2).
extern const Workload kCoreWorkloads[];
extern const int      kCoreCount;

extern const Workload kVoiceWorkloads[];
extern const int      kVoiceCount;

extern const Workload kMemWorkloads[];
extern const int      kMemCount;

extern const Workload kModWorkloads[];
extern const int      kModCount;

extern const Workload kAblWorkloads[];
extern const int      kAblCount;

// Anchor mode re-runs rows the offline tables already define, by name.
const Workload* find_workload(const char* name);

} // namespace bench
