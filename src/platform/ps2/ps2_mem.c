/*
 * EE heap instrumentation for the Fallout PS2 port.
 *
 * The engine's caches (art, SFX, db) are hard-capped by their config values,
 * but the smoke suite also wants to see the *system* allocator so leaks and
 * fragmentation in the platform layer cannot hide behind the engine's own
 * accounting. mallinfo() and sbrk() come from ps2sdk's newlib; both report
 * bytes. sbrk(0) is the heap break and never shrinks on this allocator, so
 * the delta from the first snapshot is the high-water mark of the heap.
 *
 * Only the smoke harness calls these; in release builds the code is inert.
 */

#include <malloc.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include "ps2_mem.h"

static unsigned char* s_brk_base = NULL;

void ps2_mem_init(void)
{
    if (s_brk_base == NULL) {
        s_brk_base = (unsigned char*)sbrk(0);
    }
}

int ps2_mem_used_kb(void)
{
    struct mallinfo mi = mallinfo();
    return (int)(mi.uordblks >> 10);
}

int ps2_mem_free_kb(void)
{
    struct mallinfo mi = mallinfo();
    return (int)(mi.fordblks >> 10);
}

int ps2_mem_sbrk_kb(void)
{
    unsigned char* now = (unsigned char*)sbrk(0);

    if (s_brk_base == NULL) {
        s_brk_base = now;
    }

    return (int)((now - s_brk_base) >> 10);
}

void ps2_mem_report(const char* tag)
{
    struct mallinfo mi = mallinfo();
    unsigned char* now = (unsigned char*)sbrk(0);

    if (s_brk_base == NULL) {
        s_brk_base = now;
    }

    printf("[ps2][smoke] mem: %-12s used=%uK free=%uK arena=%uK sbrk=+%uK\n",
        tag,
        (unsigned int)(mi.uordblks >> 10),
        (unsigned int)(mi.fordblks >> 10),
        (unsigned int)(mi.arena >> 10),
        (unsigned int)((now - s_brk_base) >> 10));
    fflush(stdout);
}
