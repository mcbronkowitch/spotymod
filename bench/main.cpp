#include <daisy_seed.h>
#include "report.h"
#include "workload.h"
#include "cycles.h"

#ifndef BENCH_GIT_HASH
#define BENCH_GIT_HASH "unknown"
#endif

static daisy::DaisySeed hw;

int main(void)
{
    hw.Init();                 // 480 MHz boost, caches on, SDRAM up
    hw.SetAudioBlockSize(96);
    hw.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);

    // No logger start and no host handshake: semihosting writes are synchronous
    // through the probe, so the first line cannot be lost to enumeration timing.
    bench::cycles_init();

    bench::report_begin(BENCH_GIT_HASH);
    for (int i = 0; i < bench::kCoreCount; ++i) {
        const bench::Workload& w = bench::kCoreWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
    bench::report_end();

    while (1) { hw.DelayMs(1000); }
}
