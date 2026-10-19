/*
 * Smoke-build memory guard: an interposed memcpy() that reports wild
 * destinations/sources (with the caller's return address) instead of
 * letting them corrupt emulated memory.
 *
 * PS2_DEV_SMOKE_TEST only. The smoke suite drives engine paths the desktop
 * port never crosses; a bad pointer there surfaces either as a TLB-miss
 * storm in the emulator log or as silent corruption, and the emulator's
 * exception dump shows only newlib's memcpy as the faulting PC. Logging the
 * caller's return address turns "wild memcpy" into an actionable engine
 * call site, and skipping the copy keeps the suite running long enough to
 * report it.
 *
 * The definition is linked before libc, so every engine memcpy() (including
 * compiler-generated ones) lands here. printf() is re-entrancy guarded: its
 * own memcpy calls hit the guard while it is logging.
 */

#include <stddef.h>
#include <stdio.h>

#ifdef PS2_DEV_SMOKE_TEST

#define PS2_GUARD_MAX_HITS 16

/* True when [p, p+n) lies inside EE memory the engine legitimately touches:
 * main RAM through any of its windows (kuseg 0x00100000, the 0x30000000
 * uncached-accelerated alias gsKit writes through, kseg0 0x80100000 and its
 * kseg1 0xA0000000 mirror) or the scratchpad. */
static int ps2_guard_range_ok(const void* p, size_t n)
{
    unsigned int a = (unsigned int)p;
    unsigned int b = a + (unsigned int)n;

    if (b < a) {
        return 0; /* wrapped around the address space */
    }
    if (a >= 0x00100000u && b <= 0x02000000u) {
        return 1; /* main RAM, kuseg */
    }
    if (a >= 0x20000000u && b <= 0x22000000u) {
        return 1; /* main RAM alias */
    }
    if (a >= 0x30000000u && b <= 0x32000000u) {
        return 1; /* main RAM, uncached accelerated (gsKit) */
    }
    if (a >= 0x70000000u && b <= 0x70004000u) {
        return 1; /* scratchpad */
    }
    if (a >= 0x80000000u && b <= 0x82000000u) {
        return 1; /* main RAM, kseg0 */
    }
    if (a >= 0xA0000000u && b <= 0xA2000000u) {
        return 1; /* main RAM, kseg1 (uncached) */
    }

    return 0;
}

static int s_guard_busy;
static int s_guard_hits;

void* memcpy(void* dst, const void* src, size_t n)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    if (n == 0) {
        return dst; /* zero-length copies legally pass NULL sources */
    }

    if (!s_guard_busy
        && (!ps2_guard_range_ok(dst, n) || !ps2_guard_range_ok(src, n))) {
        s_guard_busy = 1;
        if (s_guard_hits < PS2_GUARD_MAX_HITS) {
            s_guard_hits++;
            printf("[SMOKE] memcpy guard: dst=%p src=%p n=%u ra=%p (copy skipped)\n",
                dst, src, (unsigned int)n, __builtin_return_address(0));
            fflush(stdout);
        }
        s_guard_busy = 0;
        return dst;
    }

    /* Forward copy, unaligned-safe. Written by hand so the compiler cannot
     * fold it back into a memcpy() call (which would recurse). */
    if ((((unsigned int)d) | ((unsigned int)s)) & 3) {
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    } else {
        while (n >= 4) {
            *(unsigned int*)d = *(const unsigned int*)s;
            d += 4;
            s += 4;
            n -= 4;
        }
        while (n > 0) {
            *d++ = *s++;
            n--;
        }
    }

    return dst;
}

#endif /* PS2_DEV_SMOKE_TEST */
