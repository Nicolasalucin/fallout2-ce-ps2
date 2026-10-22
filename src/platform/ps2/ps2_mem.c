/*
 * EE heap + stack instrumentation for the Fallout PS2 port.
 *
 * The engine's caches (art, SFX, db) are hard-capped by their config values,
 * but the smoke suite also wants to see the *system* allocator so leaks and
 * fragmentation in the platform layer cannot hide behind the engine's own
 * accounting. mallinfo() and sbrk() come from ps2sdk's newlib; both report
 * bytes. sbrk(0) is the heap break and never shrinks on this allocator, so
 * the delta from the first snapshot is the high-water mark of the heap.
 *
 * The raw-ELF boot also has to prove that the 1 MB main stack is not
 * silently exhausted. ps2_mem_init() asks the kernel for the current thread
 * stack bounds, paints the unused lower part with 0xCD and keeps a small
 * guard at the bottom; the report later derives a high-water mark and flags
 * an overflow.
 */

#include <kernel.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ps2_debug.h"
#include "ps2_mem.h"

#define PS2_STACK_PAINT 0xCD
#define PS2_STACK_GUARD 64
#define PS2_STACK_RESERVE (64 * 1024)

static unsigned char* s_brk_base = NULL;

static uintptr_t s_stack_bottom;
static uintptr_t s_stack_top;
static uintptr_t s_paint_end;
static int s_stack_valid;

static uintptr_t mem_stack_pointer(void)
{
    return (uintptr_t)__builtin_frame_address(0);
}

static void mem_stack_setup(void)
{
    ee_thread_status_t status;
    uintptr_t sp;
    uintptr_t a;
    uintptr_t bottom;
    uintptr_t top;
    int size;

    memset(&status, 0, sizeof(status));

    /* TH_SELF is explicitly supported by ReferThreadStatus(). */
    if (ReferThreadStatus(TH_SELF, &status) < 0) {
        return;
    }

    a = (uintptr_t)status.stack;
    size = status.stack_size;
    if (a == 0 || size <= 0 || size > 8 * 1024 * 1024) {
        return;
    }

    sp = mem_stack_pointer();

    /* The EE kernel reports the stack top on current BIOS versions; older
     * ones report the base. Accept either orientation, but only if SP is
     * actually inside the range (that rules out a bogus status block). */
    bottom = a - (uintptr_t)size;
    top = a;
    if (sp < bottom || sp > top) {
        bottom = a;
        top = a + (uintptr_t)size;
    }
    if (sp < bottom || sp > top || top <= bottom) {
        return;
    }

    s_stack_bottom = bottom;
    s_stack_top = top;
    s_stack_valid = 1;

    /* Do not touch the active frames above SP. Everything below SP-64K is
     * unused at this point and is safe to paint. If the stack is already
     * nearly exhausted, skip the paint and let the report say so. */
    if (sp > bottom + PS2_STACK_RESERVE) {
        uintptr_t end = sp - PS2_STACK_RESERVE;

        memset((void*)bottom, PS2_STACK_PAINT, (size_t)(end - bottom));
        s_paint_end = end;
    }
}

void ps2_mem_init(void)
{
    if (s_brk_base == NULL) {
        s_brk_base = (unsigned char*)sbrk(0);
    }

    if (!s_stack_valid) {
        mem_stack_setup();
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

void ps2_mem_stack_report(void)
{
    uintptr_t first = 0;
    int canary_ok = 0;
    unsigned int used_kb = 0;
    unsigned int size_kb;

    if (!s_stack_valid) {
        PS2_LOG("[ps2] stack: bounds unavailable (ReferThreadStatus failed)");
        return;
    }

    size_kb = (unsigned int)((s_stack_top - s_stack_bottom) >> 10);

    if (s_paint_end > s_stack_bottom) {
        const unsigned char* p = (const unsigned char*)s_stack_bottom;
        const unsigned char* end = (const unsigned char*)s_paint_end;

        while (p < end && *p == PS2_STACK_PAINT) {
            p++;
        }
        first = (uintptr_t)p;

        if (first < s_stack_bottom + PS2_STACK_GUARD) {
            /* The guard bytes were overwritten: the stack reached the very
             * bottom of its allocation. */
            canary_ok = 0;
        } else {
            canary_ok = 1;
        }

        if (first < s_stack_top) {
            used_kb = (unsigned int)((s_stack_top - first) >> 10);
        } else {
            used_kb = 0;
        }
    } else {
        /* Painting was skipped because SP was too close to the bottom. */
        canary_ok = 0;
        used_kb = size_kb;
    }

    PS2_LOG("[ps2] stack: top=0x%08x bottom=0x%08x size=%uK used>=%uK canary=%s",
        (unsigned)s_stack_top, (unsigned)s_stack_bottom, size_kb, used_kb,
        canary_ok ? "ok" : "OVERFLOW/UNPAINTED");
}
