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
    // BOOT_SRAM loads jump straight into the app's reset vector via the debug
    // probe (openocd/spotykach-sram.cfg), bypassing the Daisy bootloader
    // entirely. DaisySeed::Init() infers "already chained from an old
    // (<v6.0) bootloader that brought SDRAM up itself" from a stale/blank
    // .backup_sram boot_info marker, and on that inference SKIPS both
    // ConfigureClocks()/ConfigureMpu() (System::Config::skip_clocks) AND
    // sdram_handle.Init() outright -- the same "bootloader normally does
    // this" gap as the FPU CPACR fix already carried in
    // openocd/spotykach-sram.cfg, just for SDRAM instead of the FPU.
    // Stamping boot_info to a recognized real-bootloader version before
    // Init() defeats that inference so both steps run for real. Confirmed
    // on hardware 2026-07-18: without this, the first workload to touch
    // SDRAM (fx_grit's Flux::init(), which memsets its injected echo
    // buffer) hits a HardFault -- SCB->CFSR IMPRECISERR, traced via
    // arm-none-eabi-addr2line into libDaisy's HardFault_Handler.
    daisy::System::InitBackupSram();
    daisy::boot_info.version = daisy::System::BootInfo::Version::v6_1;

    hw.Init(true);              // 480 MHz boost, caches on, SDRAM up
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
    for (int i = 0; i < bench::kVoiceCount; ++i) {
        const bench::Workload& w = bench::kVoiceWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
    for (int i = 0; i < bench::kMemCount; ++i) {
        const bench::Workload& w = bench::kMemWorkloads[i];
        bench::report_row(w, bench::run_workload(w));
    }
    bench::report_end();

    while (1) { hw.DelayMs(1000); }
}
