/*
 * Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/arch/cpu.h>

/* HCR bit 1 is RES1 on Cortex-R52. */
#define HCR_RES1		(1 << 1)
#define HCPTR_TCP10_TCP11	0xc00
#define HCPTR_TTA		(1 << 20)
#define HCPTR_TCPAC		(1 << 31)
#define CNTHCTL_EL1PCTEN_EL1PCEN	3
#define PWRSEQ_WDOG_CTL		0xf1000404

	.syntax unified
	.arm
	.section .sail_entry, "ax"
	.balign 32
	.global sail_reset
	.type sail_reset, %function
sail_reset:
	b .Lreset
	.rept 7
	b .Lpark
	.endr
.Lreset:
	cpsid aif
	/* Establish EL2 state before entering the EL1 kernel. */
	ldr r1, =sail_reset
	mcr p15, 4, r1, c12, c0, 0
	/* RES1 bits only: MPU, alignment check and caches start disabled. */
	ldr r1, =HSCTLR_RES1
	mcr p15, 4, r1, c1, c0, 0
	dsb sy
	isb
	mrc p15, 0, r1, c0, c0, 5
	ubfx r1, r1, #0, #24
	cmp r1, #0
	bne .Lpark

	/* Same value in SCTLR: nTWI and nTWE (bits 16, 18) set, so WFI and WFE
	 * are not trapped; the other bits set are RES1. MPU, alignment check and
	 * caches stay off.
	 */
	ldr r1, =HSCTLR_RES1
	mcr p15, 0, r1, c1, c0, 0
	mov r1, #HCR_RES1
	mcr p15, 4, r1, c1, c1, 0
	mrc p15, 4, r1, c1, c1, 2
	bic r1, r1, #HCPTR_TCP10_TCP11
	bic r1, r1, #HCPTR_TTA
	bic r1, r1, #HCPTR_TCPAC
	mcr p15, 4, r1, c1, c1, 2
	mov r1, #0
	mcr p15, 4, r1, c1, c1, 3
	mov r1, #CNTHCTL_EL1PCTEN_EL1PCEN
	mcr p15, 4, r1, c14, c1, 0
	/* CNTFRQ is writable only at EL2 on Armv8-R. */
	ldr r1, =CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
	mcr p15, 0, r1, c14, c0, 0
	isb
	mrc p15, 0, r2, c14, c0, 0
	cmp r2, r1
	bne .Lpark
	/* Permit the EL1 GICv3 driver to enable its system-register interface. */
	mrc p15, 4, r1, c12, c9, 5
	orr r1, r1, #(ICC_SRE_ELx_SRE_BIT | ICC_SRE_ELx_EN_BIT)
	mcr p15, 4, r1, c12, c9, 5
	dsb sy
	isb

	/* The power sequencer watchdog is not used by this firmware. */
	ldr r1, =PWRSEQ_WDOG_CTL
	mov r2, #0
	str r2, [r1]
	dsb sy
	b z_arm_reset
.Lpark:
	dsb sy
	wfi
	b .Lpark
	.ltorg
	.balign 32
	.size sail_reset, . - sail_reset
