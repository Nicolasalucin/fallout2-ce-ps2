/*
 * EE timing services for the Fallout PS2 port.
 *
 * Uses the ps2sdk kernel system timer, which counts at the EE bus clock
 * (kBUSCLK = 147456000 Hz). GetTimerSystemTime() returns a 64-bit value so
 * no wraparound handling is needed.
 *
 * If the boot banner in PCSX2 shows a wildly wrong "timebase" value, the
 * divisor below is the single place to fix.
 */

#include "ps2_time.h"

#include <timer.h>

/* EE bus clock: 147.456 MHz -> 147456 ticks per millisecond. */
#define PS2_TIMER_HZ 147456000ULL
#define PS2_TIMER_TICKS_PER_MS (PS2_TIMER_HZ / 1000)
#define PS2_TIMER_TICKS_PER_US (PS2_TIMER_HZ / 1000000)

void ps2_time_init(void)
{
    /* First read kicks the counter; subsequent reads are monotonic. */
    (void)GetTimerSystemTime();
}

uint64_t ps2_time_us(void)
{
    return GetTimerSystemTime() / PS2_TIMER_TICKS_PER_US;
}

uint32_t ps2_time_ms(void)
{
    return (uint32_t)(GetTimerSystemTime() / PS2_TIMER_TICKS_PER_MS);
}
