#include "report.h"
#include <cstdio>
#include <cstdarg>

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
    // Fixed measurement conditions, echoed so a result file is self-describing.
    logf("BENCH_BEGIN,%s,480000000,96,dcache+icache\n", githash);
}

void report_end()
{
    log_line("BENCH_END\n");
}

} // namespace bench
