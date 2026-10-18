#ifndef PS2_TIME_H_
#define PS2_TIME_H_

/*
 * EE timing services for the Fallout PS2 port.
 *
 * Backed by the kernel system timer (bus clock, 147.456 MHz), exposed as
 * milliseconds (wrapping at ~49.7 days, SDL-like) and microseconds.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ps2_time_init(void);

/* Monotonic milliseconds since boot (wraps like SDL_GetTicks). */
uint32_t ps2_time_ms(void);

/* Monotonic microseconds since boot (64-bit, no wrap). */
uint64_t ps2_time_us(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_TIME_H_ */
