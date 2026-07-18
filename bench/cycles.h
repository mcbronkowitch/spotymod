#pragma once
#include <cstdint>
#include <daisy_seed.h>   // pulls in the CMSIS core headers for CoreDebug/DWT

namespace bench {

// The Cortex-M7 DWT cycle counter. Free-running at the core clock (480 MHz),
// so one tick is one cycle and the block budget is a plain integer compare.
// The M7 gates DWT registers behind a lock; LAR must be unlocked first, which
// the M3/M4 examples floating around the web omit.
inline void cycles_init()
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

inline uint32_t cycles_now()
{
    return DWT->CYCCNT;
}

} // namespace bench
