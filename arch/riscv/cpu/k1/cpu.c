// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024, Kongyang Liu <seashell11234455@gmail.com>
 */

#include <linux/bitops.h>

/*
 * X60 core setup CSR (M-mode only). OpenSBI's K1 platform code names it
 * MSETUP and turns everything on for the harts it brings up; the vendor
 * U-Boot's cache.c drives the same bits.
 */
#define CSR_MSETUP		0x7c0
#define MSETUP_DE		BIT(0)	/* D-cache enable */
#define MSETUP_IE		BIT(1)	/* I-cache enable */
#define MSETUP_BPE		BIT(4)	/* branch predictor enable */
#define MSETUP_PFE		BIT(5)	/* prefetch enable */

int cleanup_before_linux(void)
{
	return 0;
}

#if CONFIG_IS_ENABLED(RISCV_MMODE)
/*
 * Run the SPL with the L1 caches off.
 *
 * The BootROM hands the FSBL over with the X60's D-cache and I-cache enabled,
 * and the SPL has no cache maintenance to go with that: flush_dcache_range()
 * and invalidate_dcache_range() are no-ops here (the Zicbom block size is only
 * established by enable_caches() in U-Boot proper).  Everything the SPL
 * stages for another agent then silently stays in the D-cache -- the DDR
 * training image copied into SRAM before it is executed, the DRAM pattern
 * test's writes before their read-back, and any DMA buffer the boot device
 * driver hands to hardware.  On the OrangePi R2S that showed as LPDDR4X
 * training completing without effect and every DRAM readback returning zero.
 *
 * Disabling the caches makes every access go to memory, which is how the
 * v2026.07 chain (which cleared these bits first thing in start.S) booted this
 * board.  OpenSBI re-enables the caches for the S-mode payload.
 *
 * Naked: this runs with the boot stack already in use.  A frame here would
 * push the return address through the (still enabled) D-cache and pop it
 * from memory after the cache is off.
 */
void __attribute__((naked)) harts_early_init(void)
{
	asm volatile(
		"li	t0, %0\n"
		"csrc	%2, t0\n"
		"li	t0, %1\n"
		"csrs	%2, t0\n"
		"fence.i\n"
		"ret\n"
		:
		: "i" (MSETUP_DE | MSETUP_IE | MSETUP_BPE | MSETUP_PFE),
		  "i" (MSETUP_BPE | MSETUP_PFE),
		  "i" (CSR_MSETUP)
		: "t0", "memory");
}
#endif
