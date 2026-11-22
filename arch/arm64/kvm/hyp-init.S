/*
 * Copyright (C) 2012,2013 - ARM Ltd
 * Author: Marc Zyngier <marc.zyngier@arm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/linkage.h>

#include <asm/assembler.h>
#include <asm/kvm_arm.h>
#include <asm/kvm_mmu.h>
#include <asm/pgtable-hwdef.h>
#include <asm/sysreg.h>
#include <asm/virt.h>
#include <asm/fpsimdmacros.h>

	.text
	.pushsection	.hyp.idmap.text, "ax"

	.align	11

ENTRY(__kvm_hyp_init)
	ventry	__invalid		// Synchronous EL2t
	ventry	__invalid		// IRQ EL2t
	ventry	__invalid		// FIQ EL2t
	ventry	__invalid		// Error EL2t

	ventry	__invalid		// Synchronous EL2h
	ventry	__invalid		// IRQ EL2h
	ventry	__invalid		// FIQ EL2h
	ventry	__invalid		// Error EL2h

	ventry	__do_hyp_init		// Synchronous 64-bit EL1
	ventry	__invalid		// IRQ 64-bit EL1
	ventry	__invalid		// FIQ 64-bit EL1
	ventry	__invalid		// Error 64-bit EL1

	ventry	__invalid		// Synchronous 32-bit EL1
	ventry	__invalid		// IRQ 32-bit EL1
	ventry	__invalid		// FIQ 32-bit EL1
	ventry	__invalid		// Error 32-bit EL1

__invalid:
	b	.

	/*
	 * x0: HYP pgd
	 * x1: HYP stack
	 * x2: HYP vectors
	 * x3: per-CPU offset
	 */
__do_hyp_init:
	/* Check for a stub HVC call */
	cmp	x0, #HVC_STUB_HCALL_NR
	b.lo	__kvm_handle_stub_hvc

	phys_to_ttbr x4, x0
	msr	ttbr0_el2, x4

	mrs	x4, tcr_el1
	ldr	x5, =TCR_EL2_MASK
	and	x4, x4, x5
	mov	x5, #TCR_EL2_RES1
	orr	x4, x4, x5

	/*
	 * The ID map may be configured to use an extended virtual address
	 * range. This is only the case if system RAM is out of range for the
	 * currently configured page size and VA_BITS, in which case we will
	 * also need the extended virtual range for the HYP ID map, or we won't
	 * be able to enable the EL2 MMU.
	 *
	 * However, at EL2, there is only one TTBR register, and we can't switch
	 * between translation tables *and* update TCR_EL2.T0SZ at the same
	 * time. Bottom line: we need to use the extended range with *both* our
	 * translation tables.
	 *
	 * So use the same T0SZ value we use for the ID map.
	 */
	ldr_l	x5, idmap_t0sz
	bfi	x4, x5, TCR_T0SZ_OFFSET, TCR_TxSZ_WIDTH

	/*
	 * Set the PS bits in TCR_EL2.
	 */
	tcr_compute_pa_size x4, #TCR_EL2_PS_SHIFT, x5, x6

	msr	tcr_el2, x4

	mrs	x4, mair_el1
	msr	mair_el2, x4
	isb

	/* Invalidate the stale TLBs from Bootloader */
	tlbi	alle2
	dsb	sy

	/*
	 * Preserve all the RES1 bits while setting the default flags,
	 * as well as the EE bit on BE. Drop the A flag since the compiler
	 * is allowed to generate unaligned accesses.
	 */
	ldr	x4, =(SCTLR_EL2_RES1 | (SCTLR_ELx_FLAGS & ~SCTLR_ELx_A))
CPU_BE(	orr	x4, x4, #SCTLR_ELx_EE)
	msr	sctlr_el2, x4
	isb

	/* Set the stack and new vectors */
	kern_hyp_va	x1
	mov	sp, x1
	msr	vbar_el2, x2

	/* Set tpidr_el2 for use by HYP */
	msr	tpidr_el2, x3

	/* Hello, World! */
	eret
ENDPROC(__kvm_hyp_init)

ENTRY(__kvm_handle_stub_hvc)
	cmp	x0, #HVC_SOFT_RESTART
	b.ne	1f

	/* This is where we're about to jump, staying at EL2 */
	msr	elr_el2, x1
	mov	x0, #(PSR_F_BIT | PSR_I_BIT | PSR_A_BIT | PSR_D_BIT | PSR_MODE_EL2h)
	msr	spsr_el2, x0

	/* Shuffle the arguments, and don't come back */
	mov	x0, x2
	mov	x1, x3
	mov	x2, x4
	b	reset

1:	cmp	x0, #HVC_RESET_VECTORS
	b.ne	1f

	/*
	 * Set the HVC_RESET_VECTORS return code before entering the common
	 * path so that we do not clobber x0-x2 in case we are coming via
	 * HVC_SOFT_RESTART.
	 */
	mov	x0, xzr
reset:
	/* Reset kvm back to the hyp stub. */
	mrs	x5, sctlr_el2
	ldr	x6, =SCTLR_ELx_FLAGS
	bic	x5, x5, x6		// Clear SCTL_M and etc
	pre_disable_mmu_workaround
	msr	sctlr_el2, x5
	isb

	/* Install stub vectors */
	adr_l	x5, __hyp_stub_vectors
	msr	vbar_el2, x5
	eret

1:	/* Bad stub call */
	ldr	x0, =HVC_STUB_ERR
	eret

ENDPROC(__kvm_handle_stub_hvc)

/*
 * EL2 lending shim. A hypervisor that owns EL2 calls __gz_lend_shim at its
 * physical address with the EL2 MMU off and PSTATE.DAIF masked:
 *   x0-x3  the arguments of a __kvm_call_hyp or hyp-stub call
 *   x4     physical address of this CPU's save page
 *   x5     kernel address of this CPU's struct user_fpsimd_state
 *   x30    return address in the lender
 * The shim swaps the lender's EL2 system registers for KVM's, runs the call
 * on KVM's hyp stack and page tables, and swaps back. It returns x0 = result
 * and x1 = 1 when it stored the live FP/SIMD registers in the x5 buffer.
 *
 * With SCTLR_EL2.M clear every data access is Device-nGnRnE, so the save
 * page is accessed only here, with aligned 8-byte loads and stores, and the
 * kernel never touches it after cleaning it out of the caches.
 */
#define GZL_X19		0x00
#define GZL_SP		0x60
#define GZL_LIVE	0x68
#define GZL_G		0x080
#define GZL_K		0x180
#define GZL_SCTLR	(19 * 8)
#define GZL_KSP		(20 * 8)

.macro gzl_save base
	mrs	x9, ttbr0_el2
	str	x9, [x19, #(\base + 0)]
	mrs	x9, tcr_el2
	str	x9, [x19, #(\base + 8)]
	mrs	x9, mair_el2
	str	x9, [x19, #(\base + 16)]
	mrs	x9, vbar_el2
	str	x9, [x19, #(\base + 24)]
	mrs	x9, tpidr_el2
	str	x9, [x19, #(\base + 32)]
	mrs	x9, hcr_el2
	str	x9, [x19, #(\base + 40)]
	mrs	x9, vttbr_el2
	str	x9, [x19, #(\base + 48)]
	mrs	x9, vtcr_el2
	str	x9, [x19, #(\base + 56)]
	mrs	x9, cptr_el2
	str	x9, [x19, #(\base + 64)]
	mrs	x9, mdcr_el2
	str	x9, [x19, #(\base + 72)]
	mrs	x9, hstr_el2
	str	x9, [x19, #(\base + 80)]
	mrs	x9, cnthctl_el2
	str	x9, [x19, #(\base + 88)]
	mrs	x9, cntvoff_el2
	str	x9, [x19, #(\base + 96)]
	mrs	x9, vpidr_el2
	str	x9, [x19, #(\base + 104)]
	mrs	x9, vmpidr_el2
	str	x9, [x19, #(\base + 112)]
	mrs_s	x9, SYS_ICH_HCR_EL2
	str	x9, [x19, #(\base + 120)]
	mrs_s	x9, SYS_ICH_VMCR_EL2
	str	x9, [x19, #(\base + 128)]
	mrs_s	x9, SYS_ICH_AP0R0_EL2
	str	x9, [x19, #(\base + 136)]
	mrs_s	x9, SYS_ICH_AP1R0_EL2
	str	x9, [x19, #(\base + 144)]
.endm

.macro gzl_load base
	ldr	x9, [x19, #(\base + 0)]
	msr	ttbr0_el2, x9
	ldr	x9, [x19, #(\base + 8)]
	msr	tcr_el2, x9
	ldr	x9, [x19, #(\base + 16)]
	msr	mair_el2, x9
	ldr	x9, [x19, #(\base + 24)]
	msr	vbar_el2, x9
	ldr	x9, [x19, #(\base + 32)]
	msr	tpidr_el2, x9
	ldr	x9, [x19, #(\base + 40)]
	msr	hcr_el2, x9
	ldr	x9, [x19, #(\base + 48)]
	msr	vttbr_el2, x9
	ldr	x9, [x19, #(\base + 56)]
	msr	vtcr_el2, x9
	ldr	x9, [x19, #(\base + 64)]
	msr	cptr_el2, x9
	ldr	x9, [x19, #(\base + 72)]
	msr	mdcr_el2, x9
	ldr	x9, [x19, #(\base + 80)]
	msr	hstr_el2, x9
	ldr	x9, [x19, #(\base + 88)]
	msr	cnthctl_el2, x9
	ldr	x9, [x19, #(\base + 96)]
	msr	cntvoff_el2, x9
	ldr	x9, [x19, #(\base + 104)]
	msr	vpidr_el2, x9
	ldr	x9, [x19, #(\base + 112)]
	msr	vmpidr_el2, x9
	ldr	x9, [x19, #(\base + 120)]
	msr_s	SYS_ICH_HCR_EL2, x9
	ldr	x9, [x19, #(\base + 128)]
	msr_s	SYS_ICH_VMCR_EL2, x9
	ldr	x9, [x19, #(\base + 136)]
	msr_s	SYS_ICH_AP0R0_EL2, x9
	ldr	x9, [x19, #(\base + 144)]
	msr_s	SYS_ICH_AP1R0_EL2, x9
.endm

ENTRY(__gz_lend_shim)
	stp	x19, x20, [x4, #(GZL_X19 + 0x00)]
	stp	x21, x22, [x4, #(GZL_X19 + 0x10)]
	stp	x23, x24, [x4, #(GZL_X19 + 0x20)]
	stp	x25, x26, [x4, #(GZL_X19 + 0x30)]
	stp	x27, x28, [x4, #(GZL_X19 + 0x40)]
	stp	x29, x30, [x4, #(GZL_X19 + 0x50)]
	mov	x9, sp
	str	x9, [x4, #GZL_SP]
	mov	x19, x4
	mov	x20, x5
	mov	x21, x0
	mov	x22, x1
	mov	x23, x2
	mov	x24, x3
	mov	x25, #0				// FP stored flag
	gzl_save GZL_G
	mrs	x9, sctlr_el2
	str	x9, [x19, #(GZL_G + GZL_SCTLR)]
	adr	x9, __kvm_hyp_init		// every current-EL vector there spins
	msr	vbar_el2, x9
	isb

	ldr	x9, [x19, #GZL_LIVE]
	cbz	x9, 5f

	/* KVM's EL2 context exists on this CPU: install it. */
	gzl_load GZL_K
	isb
	tlbi	alle2
	dsb	sy
	ldr	x10, [x19, #(GZL_K + GZL_KSP)]
	ldr	x9, [x19, #(GZL_K + GZL_SCTLR)]
	msr	sctlr_el2, x9
	isb
	mov	sp, x10
	mov	x26, #1				// context stays live

	cmp	x21, #HVC_STUB_HCALL_NR
	b.lo	3f
	mov	x9, x21
	kern_hyp_va	x9
	mov	x0, x22
	mov	x1, x23
	mov	x2, x24
	blr	x9
	mov	x21, x0
	mov	x9, x20
	kern_hyp_va	x9
	fpsimd_save	x9, 10
	mov	x25, #1
	b	4f

3:	/* Stub call against a live context. */
	cmp	x21, #HVC_RESET_VECTORS
	csel	x26, xzr, x26, eq
	mov	x9, x21
	mov	x21, #0
	cmp	x9, #HVC_SOFT_RESTART
	b.ne	4f
	ldr	x21, =HVC_STUB_ERR

4:	/* Leave KVM's context: MMU off, then store it. */
	mov	x10, sp
	mrs	x11, sctlr_el2
	bic	x9, x11, #SCTLR_ELx_M
	dsb	sy
	msr	sctlr_el2, x9
	isb
	gzl_save GZL_K
	str	x11, [x19, #(GZL_K + GZL_SCTLR)]
	str	x10, [x19, #(GZL_K + GZL_KSP)]
	str	x26, [x19, #GZL_LIVE]
	b	9f

5:	/* No KVM context on this CPU yet. */
	cmp	x21, #HVC_STUB_HCALL_NR
	b.hs	6f
	mov	x9, x21
	mov	x21, #0
	cmp	x9, #HVC_SOFT_RESTART
	b.ne	9f
	ldr	x21, =HVC_STUB_ERR
	b	9f

6:	/*
	 * Hyp init: x21 = pgd, x22 = stack, x23 = vectors, x24 = per-CPU
	 * offset. Same register programming as __do_hyp_init.
	 */
	phys_to_ttbr x4, x21
	msr	ttbr0_el2, x4
	mrs	x4, tcr_el1
	ldr	x5, =TCR_EL2_MASK
	and	x4, x4, x5
	mov	x5, #TCR_EL2_RES1
	orr	x4, x4, x5
	ldr_l	x5, idmap_t0sz
	bfi	x4, x5, TCR_T0SZ_OFFSET, TCR_TxSZ_WIDTH
	tcr_compute_pa_size x4, #TCR_EL2_PS_SHIFT, x5, x6
	msr	tcr_el2, x4
	mrs	x4, mair_el1
	msr	mair_el2, x4
	mov	x4, #HCR_RW
	msr	hcr_el2, x4
	msr	vttbr_el2, xzr
	isb
	tlbi	alle2
	dsb	sy
	ldr	x4, =(SCTLR_EL2_RES1 | (SCTLR_ELx_FLAGS & ~SCTLR_ELx_A))
	msr	sctlr_el2, x4
	isb
	kern_hyp_va	x22
	mov	sp, x22
	msr	vbar_el2, x23
	msr	tpidr_el2, x24
	isb
	mov	x21, #0
	mov	x26, #1
	b	4b

9:	/* Reinstall the lender's EL2 context and return to it. */
	gzl_load GZL_G
	isb
	tlbi	alle2
	dsb	sy
	ldr	x9, [x19, #(GZL_G + GZL_SCTLR)]
	msr	sctlr_el2, x9
	isb
	ldr	x9, [x19, #GZL_SP]
	mov	sp, x9
	mov	x0, x21
	mov	x1, x25
	mov	x2, #0
	mov	x3, #0
	mov	x4, x19
	ldp	x19, x20, [x4, #(GZL_X19 + 0x00)]
	ldp	x21, x22, [x4, #(GZL_X19 + 0x10)]
	ldp	x23, x24, [x4, #(GZL_X19 + 0x20)]
	ldp	x25, x26, [x4, #(GZL_X19 + 0x30)]
	ldp	x27, x28, [x4, #(GZL_X19 + 0x40)]
	ldp	x29, x30, [x4, #(GZL_X19 + 0x50)]
	ret
ENDPROC(__gz_lend_shim)

	.ltorg

	.popsection
