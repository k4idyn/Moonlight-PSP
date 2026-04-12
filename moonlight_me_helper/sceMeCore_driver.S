// ME bootstrap stub — copied to 0xBFC00040 by InitME()
// Based on melib by mrbrown
//
// The ME processor executes this code after hardware reset.
// It initializes the ME bus controller, clears caches, then
// jumps to the C function pointer stored at 0xBFC00600 with
// the argument from 0xBFC00604.  Stack set to top of ME eDRAM.

#include <regdef.h>

#define STATUS	$12
#define CAUSE	$13
#define CONFIG	$16
#define TAGLO	$28
#define TAGHI	$29
#define IXIST	0x01
#define DXIST	0x11

	.global me_stub
	.global me_stub_end
	.set noreorder
	.set noat

me_stub:
	/* Configure ME bus controller at 0xBC100000 */
	li		k0, 0xbc100000
	li		t0, 7
	sw		t0, 80(k0)		/* Enable ME bus arbitration */
	li		t0, 2
	sw		t0, 64(k0)		/* Set ME bus priority */

	/* Clear I-cache and D-cache tags */
	mtc0	zero, TAGLO
	mtc0	zero, TAGHI

	/* Invalidate I-cache: 16 KB = 256 lines x 64 bytes */
	li		k1, 8192
_icache_inv:
	addi	k1, k1, -64
	bne		k1, zero, _icache_inv
	cache	IXIST, 0(k1)

	/* Invalidate D-cache: 8 KB = 128 lines x 64 bytes */
	li		k1, 8192
_dcache_inv:
	addi	k1, k1, -64
	bne		k1, zero, _dcache_inv
	cache	DXIST, 0(k1)

	/* Clear pending exceptions */
	mtc0	zero, CAUSE

	/* Clear ME interrupt mask registers (0xBC000000-0xBC00002C) */
	li		t0, 0xbc00002c
	li		t1, 0xbc000000
	li		t2, -1
_clr_irq:
	sw		t2, 0(t1)
	bne		t1, t0, _clr_irq
	addiu	t1, t1, 4

	/* Set STATUS: kernel mode, interrupts disabled, 32-bit mode */
	li		k0, 0x20000000
	mtc0	k0, STATUS
	sync

	/* Load function pointer (k0) and argument (a0) from SRAM */
	li		t0, 0xbfc00000
	lw		a0, 0x604(t0)	/* a0 = me_struct pointer */
	lw		k0, 0x600(t0)	/* k0 = me_loop function pointer */

	/* Set stack to top of ME eDRAM (2 MB at 0x80000000-0x801FFFFF) */
	li		sp, 0x80200000

	/* Jump to me_loop(mei) */
	jr		k0
	nop

me_stub_end:
