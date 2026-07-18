#include "report.h"
#include <cstdio>
#include <cstdarg>
#include <daisy_seed.h>   // daisy::System::GetSysClkFreq(), CMSIS SCB/SCB_CCR_*_Msk

namespace bench {
namespace {

// ARM semihosting SYS_WRITE0: r0 = op, r1 = pointer to a NUL-terminated
// string. The bkpt is the call. This is the whole transport.
constexpr int kSysWrite0 = 0x04;

inline void sh_write0(const char* s)
{
    register int         r0 asm("r0") = kSysWrite0;
    register const char* r1 asm("r1") = s;
    asm volatile("bkpt 0xAB" : "+r"(r0) : "r"(r1) : "memory");
}

char g_buf[256];

} // namespace

void log_line(const char* s)
{
    sh_write0(s);
}

void logf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_buf, sizeof(g_buf), fmt, ap);
    va_end(ap);
    sh_write0(g_buf);
}

void report_begin(const char* githash)
{
    // Measured, not asserted: this is exactly the failure mode that already
    // bit this project once (the plan claimed 480 MHz while hw.Init() was
    // actually delivering something else). Read the real clock and the real
    // cache-enable bits so the header line describes what is actually true.
    const uint32_t clk = daisy::System::GetSysClkFreq();
    const uint32_t ccr = SCB->CCR;
    const bool     ic  = (ccr & SCB_CCR_IC_Msk) != 0;
    const bool     dc  = (ccr & SCB_CCR_DC_Msk) != 0;
    const char* cache = (ic && dc) ? "dcache+icache"
                      : ic         ? "icache"
                      : dc         ? "dcache"
                                   : "none";
    logf("BENCH_BEGIN,%s,%lu,96,%s\n", githash, (unsigned long)clk, cache);
}

void report_end()
{
    log_line("BENCH_END\n");
}

} // namespace bench

namespace {
// Percent of the block budget in hundredths, printed as a fixed-point pair.
// Integer maths keeps the output exact and keeps float formatting (and its
// newlib bulk) out of the binary.
inline uint32_t pct_x100(uint32_t cyc)
{
    return static_cast<uint32_t>((static_cast<uint64_t>(cyc) * 10000ull)
                                 / bench::kBudgetCycles);
}
} // namespace

namespace bench {

void report_row(const Workload& w, const Result& r)
{
    if (r.timed_out) {
        logf("BENCH,%s,%s,TIMEOUT,%lu,,,%08lx\n",
             w.family, w.name,
             (unsigned long)r.max_cyc, (unsigned long)r.checksum);
        return;
    }
    const uint32_t pa = pct_x100(r.avg_cyc);
    const uint32_t pm = pct_x100(r.max_cyc);
    logf("BENCH,%s,%s,%lu,%lu,%lu.%02lu,%lu.%02lu,%08lx\n",
         w.family, w.name,
         (unsigned long)r.avg_cyc, (unsigned long)r.max_cyc,
         (unsigned long)(pa / 100), (unsigned long)(pa % 100),
         (unsigned long)(pm / 100), (unsigned long)(pm % 100),
         (unsigned long)r.checksum);
}

} // namespace bench
