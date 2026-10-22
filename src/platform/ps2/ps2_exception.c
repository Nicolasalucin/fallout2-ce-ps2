/*
 * EE exception handler + crash screen for the Fallout PS2 port.
 *
 * The assembly vector (ps2_exception_asm.S) saves the interrupted R5900
 * state into ps2_exception_frame, switches to ps2_exception_stack and calls
 * ps2_exception_report(). Everything here must be safe with a corrupt stack
 * and with the IOP/GS possibly wedged:
 *
 *   - the screen is drawn through libdebug's raw GS/DMA code, not gsKit;
 *   - no allocation, no stdio FILE*, no directory scans;
 *   - the debug.log write is best-effort and happens only after the screen
 *     is already visible.
 */

#include "ps2_exception.h"
#include "ps2_debug.h"
#include "ps2_time.h"

#include "ps2sdk_debug.h"
#include <delaythread.h>
#include <kernel.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PS2_EXCEPTION_STEPS 16
#define PS2_EXCEPTION_TAG_MAX 24

typedef struct ps2_exception_step_entry {
    uint32_t ms;
    uint32_t value;
    char tag[PS2_EXCEPTION_TAG_MAX];
} ps2_exception_step_entry_t;

static ps2_exception_step_entry_t s_steps[PS2_EXCEPTION_STEPS];
static volatile uint32_t s_step_seq;
static volatile int s_in_report;

/* Referenced by ps2_exception_asm.S. 32 KB is enough for scr_printf's
 * vsnprintf and the libdebug font path even if the interrupted stack is
 * unusable. */
ps2_exception_frame_t ps2_exception_frame __attribute__((aligned(128)));
unsigned char ps2_exception_stack[32 * 1024] __attribute__((aligned(128)));

static const char* exception_name(unsigned int code)
{
    switch (code) {
    case 1:
        return "TLB modification";
    case 2:
        return "TLB load miss";
    case 3:
        return "TLB store miss";
    case 4:
        return "address error (load)";
    case 5:
        return "address error (store)";
    case 6:
        return "instruction bus error";
    case 7:
        return "data bus error";
    case 8:
        return "syscall";
    case 9:
        return "breakpoint";
    case 10:
        return "reserved instruction";
    case 11:
        return "coprocessor unusable";
    case 12:
        return "overflow";
    case 13:
        return "trap";
    default:
        return "unknown";
    }
}

void ps2_exception_step(const char* tag, unsigned int value)
{
    uint32_t seq = s_step_seq;
    ps2_exception_step_entry_t* entry = &s_steps[seq % PS2_EXCEPTION_STEPS];

    if (tag == NULL) {
        tag = "?";
    }

    entry->ms = ps2_time_ms();
    entry->value = (uint32_t)value;
    strncpy(entry->tag, tag, sizeof(entry->tag) - 1);
    entry->tag[sizeof(entry->tag) - 1] = '\0';

    s_step_seq = seq + 1;
}

void ps2_exception_init(void)
{
    int i;

    for (i = 1; i <= 3; i++) {
        SetVTLBRefillHandler(i, (void*)ps2_exception_vector);
    }
    for (i = 4; i <= 7; i++) {
        SetVCommonHandler(i, (void*)ps2_exception_vector);
    }
    for (i = 10; i <= 13; i++) {
        SetVCommonHandler(i, (void*)ps2_exception_vector);
    }

    PS2_LOG("[ps2] exception: R5900 vectors installed");
}

void ps2_exception_report(ps2_exception_frame_t* f)
{
    uint32_t code;
    uint32_t seq;
    uint32_t first;
    uint32_t count;
    uint32_t i;

    if (s_in_report) {
        for (;;) {
            __asm__ __volatile__("nop");
        }
    }
    s_in_report = 1;

    if (f == NULL) {
        for (;;) {
            __asm__ __volatile__("nop");
        }
    }

    code = (f->cause >> 2) & 0x1F;

    /* Draw first: this path only touches the GS/DMA, so it still works when
     * the IOP, the USB stick or the memory card are gone. */
    init_scr();
    scr_clear();
    scr_setbgcolor(0x000000);
    scr_setfontcolor(0xFFFFFF);

    scr_printf("FALLOUT 2 PS2 - CPU EXCEPTION\n");
    scr_printf("---------------------------\n\n");
    scr_printf("cause %u: %s\n", (unsigned)code, exception_name(code));
    scr_printf("EPC       %08x\n", (unsigned)f->epc);
    scr_printf("BadVAddr  %08x\n", (unsigned)f->badvaddr);
    scr_printf("Status    %08x\n", (unsigned)f->status);
    scr_printf("Cause     %08x\n", (unsigned)f->cause);
    scr_printf("ErrorEPC  %08x\n", (unsigned)f->errorepc);
    scr_printf("RA        %08x\n", (unsigned)f->ra[0]);
    scr_printf("SP        %08x\n", (unsigned)f->sp[0]);
    scr_printf("GP        %08x\n", (unsigned)f->gp[0]);
    scr_printf("a0        %08x\n", (unsigned)f->a0[0]);
    scr_printf("a1        %08x\n", (unsigned)f->a1[0]);
    scr_printf("v0        %08x\n", (unsigned)f->v0[0]);
    scr_printf("hi/lo     %08x / %08x\n\n", (unsigned)f->hi, (unsigned)f->lo);

    scr_printf("Last engine steps:\n");
    seq = s_step_seq;
    count = seq < PS2_EXCEPTION_STEPS ? seq : PS2_EXCEPTION_STEPS;
    first = seq - count;
    for (i = 0; i < count; i++) {
        const ps2_exception_step_entry_t* entry =
            &s_steps[(first + i) % PS2_EXCEPTION_STEPS];

        scr_printf(" %8u ms  %-20s %08x\n",
            (unsigned)entry->ms, entry->tag, (unsigned)entry->value);
    }

    scr_printf("\nThe console is halted. Reset it to boot again.\n");
    scr_printf("The same data is in FALLOUT2/debug.log.\n");

    /* Best effort, after the screen is visible. If fileio is wedged this may
     * block, but the user can already read the crash. */
    PS2_LOG("[ps2] EXCEPTION cause=%u (%s) epc=%08x badvaddr=%08x status=%08x",
        (unsigned)code, exception_name(code), (unsigned)f->epc,
        (unsigned)f->badvaddr, (unsigned)f->status);
    PS2_LOG("[ps2] EXCEPTION ra=%08x sp=%08x gp=%08x a0=%08x a1=%08x",
        (unsigned)f->ra[0], (unsigned)f->sp[0], (unsigned)f->gp[0],
        (unsigned)f->a0[0], (unsigned)f->a1[0]);
    for (i = 0; i < count; i++) {
        const ps2_exception_step_entry_t* entry =
            &s_steps[(first + i) % PS2_EXCEPTION_STEPS];

        PS2_LOG("[ps2] step %8u ms %-20s %08x",
            (unsigned)entry->ms, entry->tag, (unsigned)entry->value);
    }

    ps2_debug_enter_crash_mode();

    for (;;) {
        __asm__ __volatile__("nop");
    }
}
