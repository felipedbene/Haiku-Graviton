/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */

/*	AArch64 TLS descriptor resolver (R_AARCH64_TLSDESC).

	The TLS descriptor ABI (AAELF64, "TLS descriptors"; also described in
	Oliva/Botcazou/Glauber, "Speeding Up Thread-Local Storage Access in Dynamic
	Libraries", 2005) has the compiler emit, for an access to a TLS variable:

		adrp	x0, :tlsdesc:var
		ldr		x1, [x0, :tlsdesc_lo12:var]
		add		x0, x0, :tlsdesc_lo12:var
		blr		x1
		mrs		xN, tpidr_el0
		add		xN, xN, x0				// &var

	x0 points to a two-word descriptor { resolver, argument } that the dynamic
	loader fills in. The resolver must return in x0 the variable's address
	minus tpidr_el0, and must preserve every other register: the call site
	treats it as clobbering only x0 (and x30, which blr writes), so the
	compiler keeps live values in caller-saved registers across it.

	DeBeOS has no static TLS block: every module's TLS lives in a block that
	the dynamic thread vector allocates on first use (elf_tls.cpp). So there is
	only one resolver here, the dynamic one; the descriptor's argument word is
	the module id and the offset packed by arch_relocate.cpp
	(arch_tlsdesc_argument()), and the address comes from the same
	get_tls_address() that __tls_get_addr() uses.

	The C path may clobber any caller-saved register, so they are all saved:
	x1-x18, x29/x30, the full 128-bit q0-q31 (AAPCS64 only preserves the low 64
	bits of v8-v15), NZCV and FPSR. The upper SVE bits of the z registers are
	not preserved: a 128-bit write to a v register zeroes them. That matters
	only for callers holding live SVE state across a TLS access; the C path
	only runs on the slow first-access-per-thread allocation for SIMD use.
*/

#include <asm_defs.h>


/* Frame layout. The 64-bit-pair stores reach only offsets 0..504, so the
   flags and general registers come first and the 128-bit pairs (0..1008)
   last. */
#define FLAG_OFF	0				/* nzcv, fpsr */
#define GPR_OFF		16				/* x1-x18 */
#define VEC_OFF		(GPR_OFF + 18 * 8)		/* q0-q31, at 160 */
#define FRAME_SIZE	(VEC_OFF + 32 * 16)		/* 672, 16-aligned */


/* addr_t __arm64_tlsdesc_dynamic(struct tlsdesc* descriptor) */
FUNCTION(__arm64_tlsdesc_dynamic):
	stp		x29, x30, [sp, #-16]!
	mov		x29, sp
	sub		sp, sp, #FRAME_SIZE

	stp		x1, x2, [sp, #(GPR_OFF + 0)]
	stp		x3, x4, [sp, #(GPR_OFF + 16)]
	stp		x5, x6, [sp, #(GPR_OFF + 32)]
	stp		x7, x8, [sp, #(GPR_OFF + 48)]
	stp		x9, x10, [sp, #(GPR_OFF + 64)]
	stp		x11, x12, [sp, #(GPR_OFF + 80)]
	stp		x13, x14, [sp, #(GPR_OFF + 96)]
	stp		x15, x16, [sp, #(GPR_OFF + 112)]
	stp		x17, x18, [sp, #(GPR_OFF + 128)]

	stp		q0, q1, [sp, #(VEC_OFF + 0 * 32)]
	stp		q2, q3, [sp, #(VEC_OFF + 1 * 32)]
	stp		q4, q5, [sp, #(VEC_OFF + 2 * 32)]
	stp		q6, q7, [sp, #(VEC_OFF + 3 * 32)]
	stp		q8, q9, [sp, #(VEC_OFF + 4 * 32)]
	stp		q10, q11, [sp, #(VEC_OFF + 5 * 32)]
	stp		q12, q13, [sp, #(VEC_OFF + 6 * 32)]
	stp		q14, q15, [sp, #(VEC_OFF + 7 * 32)]
	stp		q16, q17, [sp, #(VEC_OFF + 8 * 32)]
	stp		q18, q19, [sp, #(VEC_OFF + 9 * 32)]
	stp		q20, q21, [sp, #(VEC_OFF + 10 * 32)]
	stp		q22, q23, [sp, #(VEC_OFF + 11 * 32)]
	stp		q24, q25, [sp, #(VEC_OFF + 12 * 32)]
	stp		q26, q27, [sp, #(VEC_OFF + 13 * 32)]
	stp		q28, q29, [sp, #(VEC_OFF + 14 * 32)]
	stp		q30, q31, [sp, #(VEC_OFF + 15 * 32)]

	mrs		x1, nzcv
	mrs		x2, fpsr
	stp		x1, x2, [sp, #FLAG_OFF]

	/* x0 = descriptor; its second word is the packed module/offset */
	ldr		x0, [x0, #8]
	bl		__arm64_tlsdesc_address

	/* return &var - tpidr_el0 */
	mrs		x1, tpidr_el0
	sub		x0, x0, x1

	ldp		x1, x2, [sp, #FLAG_OFF]
	msr		nzcv, x1
	msr		fpsr, x2

	ldp		q0, q1, [sp, #(VEC_OFF + 0 * 32)]
	ldp		q2, q3, [sp, #(VEC_OFF + 1 * 32)]
	ldp		q4, q5, [sp, #(VEC_OFF + 2 * 32)]
	ldp		q6, q7, [sp, #(VEC_OFF + 3 * 32)]
	ldp		q8, q9, [sp, #(VEC_OFF + 4 * 32)]
	ldp		q10, q11, [sp, #(VEC_OFF + 5 * 32)]
	ldp		q12, q13, [sp, #(VEC_OFF + 6 * 32)]
	ldp		q14, q15, [sp, #(VEC_OFF + 7 * 32)]
	ldp		q16, q17, [sp, #(VEC_OFF + 8 * 32)]
	ldp		q18, q19, [sp, #(VEC_OFF + 9 * 32)]
	ldp		q20, q21, [sp, #(VEC_OFF + 10 * 32)]
	ldp		q22, q23, [sp, #(VEC_OFF + 11 * 32)]
	ldp		q24, q25, [sp, #(VEC_OFF + 12 * 32)]
	ldp		q26, q27, [sp, #(VEC_OFF + 13 * 32)]
	ldp		q28, q29, [sp, #(VEC_OFF + 14 * 32)]
	ldp		q30, q31, [sp, #(VEC_OFF + 15 * 32)]

	ldp		x1, x2, [sp, #(GPR_OFF + 0)]
	ldp		x3, x4, [sp, #(GPR_OFF + 16)]
	ldp		x5, x6, [sp, #(GPR_OFF + 32)]
	ldp		x7, x8, [sp, #(GPR_OFF + 48)]
	ldp		x9, x10, [sp, #(GPR_OFF + 64)]
	ldp		x11, x12, [sp, #(GPR_OFF + 80)]
	ldp		x13, x14, [sp, #(GPR_OFF + 96)]
	ldp		x15, x16, [sp, #(GPR_OFF + 112)]
	ldp		x17, x18, [sp, #(GPR_OFF + 128)]

	add		sp, sp, #FRAME_SIZE
	ldp		x29, x30, [sp], #16
	ret
FUNCTION_END(__arm64_tlsdesc_dynamic)
