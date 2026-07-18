#pragma once

#include "workload.h"

namespace bench {

// Output leaves over the SWD link the probe already owns (ARM semihosting),
// not over a second USB cable -- see the plan's deviation note 1. openocd
// services the breakpoint and prints the string on its own stdout.
//
// DELIBERATE CONSEQUENCE: this binary requires an attached openocd. Without
// one, the first bkpt 0xAB halts the core forever. Fine here -- the bench is
// never shipped and runs by definition under the probe.
void log_line(const char* s);
void logf(const char* fmt, ...);

// Marker contract, parsed by run.py. Anything printed outside these markers
// is free-form and ignored by the parser.
void report_begin(const char* githash);
void report_end();

void report_row(const Workload& w, const Result& r);

} // namespace bench
