/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EL2 callee for the lending probe. It runs at its physical address with the
 * EL2 MMU off, uses no stack and no memory, and clobbers x0-x3 only.
 * Returns CurrentEL, SCTLR_EL2, its own address and a signature.
 */
#include <linux/linkage.h>

	.text
	.align	3
ENTRY(gz_lend_probe)
	mrs	x0, CurrentEL
	mrs	x1, sctlr_el2
	adr	x2, gz_lend_probe
	movz	x3, #0x4159
	movk	x3, #0x4f4b, lsl #16
	movk	x3, #0x4e44, lsl #32
	movk	x3, #0x4c45, lsl #48
	ret
ENDPROC(gz_lend_probe)
