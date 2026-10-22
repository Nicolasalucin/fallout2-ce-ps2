/*
 * R5900 exception vector for the Fallout 2 PS2 port.
 *
 * Entered directly by the EE kernel handlers installed with
 * SetVTLBRefillHandler()/SetVCommonHandler(): the interrupted registers and
 * stack are still live. Copy them into ps2_exception_frame, switch to the
 * dedicated exception stack and call the C reporter. The reporter never
 * returns, so there is no eret path here.
 */

/* Force the EE register aliases. The binutils default for this target is
 * N32, where $t4-$t7 do not exist and $t0-$t3 mean GPR 12-15. The C ABI is
 * EABI64, so the macros must come from as_reg_compat.h. */
#define ABI_EABI64
#include <as_reg_compat.h>

#include <ee_cop0_defs.h>

    .text
    .set push
    .set noreorder
    .set noat

    .global ps2_exception_vector
    .ent ps2_exception_vector
ps2_exception_vector:
    lui         $k0, %hi(ps2_exception_frame)
    addiu       $k0, $k0, %lo(ps2_exception_frame)

    /* Preserve the registers that are needed to rebuild the frame before
     * the vector starts using them. */
    sq          $at, 0x010($k0)
    sq          $s0, 0x100($k0)
    sq          $sp, 0x1D0($k0)
    sq          $ra, 0x1F0($k0)

    /* Switch to the private stack; the interrupted one may be the reason
     * we are here. */
    lui         $sp, %hi(ps2_exception_stack + 0x8000)
    addiu       $sp, $sp, %lo(ps2_exception_stack + 0x8000)

    move        $s0, $k0

    sq          $zero, 0x000($s0)
    sq          $v0, 0x020($s0)
    sq          $v1, 0x030($s0)
    sq          $a0, 0x040($s0)
    sq          $a1, 0x050($s0)
    sq          $a2, 0x060($s0)
    sq          $a3, 0x070($s0)
    sq          $t0, 0x080($s0)
    sq          $t1, 0x090($s0)
    sq          $t2, 0x0A0($s0)
    sq          $t3, 0x0B0($s0)
    sq          $t4, 0x0C0($s0)
    sq          $t5, 0x0D0($s0)
    sq          $t6, 0x0E0($s0)
    sq          $t7, 0x0F0($s0)
    sq          $s1, 0x110($s0)
    sq          $s2, 0x120($s0)
    sq          $s3, 0x130($s0)
    sq          $s4, 0x140($s0)
    sq          $s5, 0x150($s0)
    sq          $s6, 0x160($s0)
    sq          $s7, 0x170($s0)
    sq          $t8, 0x180($s0)
    sq          $t9, 0x190($s0)

    /* k0 is the frame pointer now; keep the slot zeroed for readability. */
    sq          $zero, 0x1A0($s0)
    sq          $k1, 0x1B0($s0)
    sq          $gp, 0x1C0($s0)
    sq          $fp, 0x1E0($s0)

    mfc0        $t1, EE_COP0_Status
    sw          $t1, 0x200($s0)
    mfc0        $t1, EE_COP0_Cause
    sw          $t1, 0x204($s0)
    mfc0        $t1, EE_COP0_EPC
    sw          $t1, 0x208($s0)
    mfc0        $t1, EE_COP0_ErrorEPC
    sw          $t1, 0x20C($s0)
    mfc0        $t1, EE_COP0_BadVAddr
    sw          $t1, 0x210($s0)

    mfhi        $t1
    sw          $t1, 0x214($s0)
    mfhi1       $t1
    sw          $t1, 0x218($s0)
    mflo        $t1
    sw          $t1, 0x21C($s0)
    mflo1       $t1
    sw          $t1, 0x220($s0)

    /* C code is built with -G0, but keep gp valid for any libc helper that
     * still assumes the small-data base. */
    la          $gp, _gp

    jal         ps2_exception_report
    move        $a0, $s0

1:
    b           1b
    nop

    .end ps2_exception_vector

    .set pop
