#ifndef PS2_EXCEPTION_H_
#define PS2_EXCEPTION_H_

/*
 * EE exception handler for the Fallout 2 PS2 port.
 *
 * The R5900 vectors installed through SetVTLBRefillHandler() and
 * SetVCommonHandler() run with the interrupted stack and no saved frame.
 * ps2_exception_asm.S provides a small vector that copies the CPU state into
 * a static frame, switches to a dedicated exception stack and calls
 * ps2_exception_report() in C. The report draws the crash on the GS through
 * libdebug (no gsKit, no file I/O) and then halts the EE.
 *
 * The frame layout is defined here instead of using ps2sdk's EE_RegFrame
 * because the port also has a local "ps2_debug.h" (the log layer), which
 * shadows the SDK header of the same name on the include path.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps2_exception_frame {
    uint32_t zero[4];
    uint32_t at[4];
    uint32_t v0[4];
    uint32_t v1[4];
    uint32_t a0[4];
    uint32_t a1[4];
    uint32_t a2[4];
    uint32_t a3[4];
    uint32_t t0[4];
    uint32_t t1[4];
    uint32_t t2[4];
    uint32_t t3[4];
    uint32_t t4[4];
    uint32_t t5[4];
    uint32_t t6[4];
    uint32_t t7[4];
    uint32_t s0[4];
    uint32_t s1[4];
    uint32_t s2[4];
    uint32_t s3[4];
    uint32_t s4[4];
    uint32_t s5[4];
    uint32_t s6[4];
    uint32_t s7[4];
    uint32_t t8[4];
    uint32_t t9[4];
    uint32_t k0[4];
    uint32_t k1[4];
    uint32_t gp[4];
    uint32_t sp[4];
    uint32_t fp[4];
    uint32_t ra[4];

    uint32_t status;   /* 0x200 */
    uint32_t cause;    /* 0x204 */
    uint32_t epc;      /* 0x208 */
    uint32_t errorepc; /* 0x20C */
    uint32_t badvaddr; /* 0x210 */
    uint32_t hi;       /* 0x214 */
    uint32_t hi1;      /* 0x218 */
    uint32_t lo;       /* 0x21C */
    uint32_t lo1;      /* 0x220 */
    uint32_t sa;
} ps2_exception_frame_t;

/* Installs the vectors for TLB/address/bus/RI/overflow/trap exceptions.
 * Call once, as early as possible in ps2_boot(). */
void ps2_exception_init(void);

/* Records a short breadcrumb in a fixed ring. Cheap enough to call on every
 * boot stage, state transition and every few frames in the game loop; the
 * crash screen prints the last entries. */
void ps2_exception_step(const char* tag, unsigned int value);

/* C entry point called by the assembly vector. Does not return. */
void ps2_exception_report(ps2_exception_frame_t* frame) __attribute__((noreturn));

/* Implemented in ps2_exception_asm.S. */
void ps2_exception_vector(void);

#ifdef __cplusplus
}
#endif

#endif /* PS2_EXCEPTION_H_ */
