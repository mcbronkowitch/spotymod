#include <daisy_seed.h>
#include "report.h"

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
    bench::report_begin(BENCH_GIT_HASH);
    bench::report_end();

    while (1) { hw.DelayMs(1000); }
}
