/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <arch/arm/arch_cpu.h>
#include <asm_defs.h>
#include "asm_offsets.h"
#include "syscall_numbers.h"

// The EL0 FP save/restore helpers use SVE stores/loads (STR/LDR Zn/Pn, RDFFR/
// WRFFR). Enabling the extension does not change the encoding of any base or
// NEON instruction elsewhere in this file.
.arch_extension sve

.text

FUNCTION(_thread_exit_syscall):
	svc #(SYSCALL_EXIT_THREAD)
FUNCTION_END(_thread_exit_syscall)

.macro xchg_sp xt
add	sp, sp, \xt
sub	\xt, sp, \xt
sub	sp, sp, \xt
.endm

.macro EXCEPTION_ENTRY el
	// interrupts are automatically disabled by hardware

	// avoid using sp in case it is misaligned
	// swap sp with x19 and use it instead
	xchg_sp x19

	// x19 is now the stack top, make room for IFRAME
	sub x19, x19, #(IFRAME_sizeof)

	stp	    x0,  x1, [x19, #(IFRAME_x + 0 * 8)]
	stp	    x2,  x3, [x19, #(IFRAME_x + 2 * 8)]
	stp	    x4,  x5, [x19, #(IFRAME_x + 4 * 8)]
	stp	    x6,  x7, [x19, #(IFRAME_x + 6 * 8)]
	stp	    x8,  x9, [x19, #(IFRAME_x + 8 * 8)]
	stp	   x10, x11, [x19, #(IFRAME_x + 10 * 8)]
	stp	   x12, x13, [x19, #(IFRAME_x + 12 * 8)]
	stp	   x14, x15, [x19, #(IFRAME_x + 14 * 8)]
	stp	   x16, x17, [x19, #(IFRAME_x + 16 * 8)]
	mov    x0,   sp  // original x19 that we swapped with sp
	stp	   x18,  x0, [x19, #(IFRAME_x + 18 * 8)]
	stp	   x20, x21, [x19, #(IFRAME_x + 20 * 8)]
	stp	   x22, x23, [x19, #(IFRAME_x + 22 * 8)]
	stp	   x24, x25, [x19, #(IFRAME_x + 24 * 8)]
	stp	   x26, x27, [x19, #(IFRAME_x + 26 * 8)]
	stp	   x28, fp,  [x19, #(IFRAME_x + 28 * 8)]
	str	   x30,      [x19, #(IFRAME_lr)]

	add x0, x19, #(IFRAME_fpu)
.if \el == 0
	// EL0 entry: also spill the wide SVE state to this thread's off-stack buffer.
	bl	_fp_save_el0
.else
	// EL1 entry: the kernel never runs SVE, so only the NEON view is saved.
	bl	_fp_save
.endif

.if \el == 0
	mrs x0, SP_EL0
	add x20, x19, #(IFRAME_sizeof)
.else
	// add sizeof back here to store original sp
	add x0, x19, #(IFRAME_sizeof)
.endif

	mrs x1, ELR_EL1
	mrs x2, SPSR_EL1
	mrs x3, ESR_EL1
	mrs x4, FAR_EL1
	mrs x5, TPIDR_EL0

	str x0, [x19, #(IFRAME_sp)]
	str x1, [x19, #(IFRAME_elr)]
	str x2, [x19, #(IFRAME_spsr)]
	str x3, [x19, #(IFRAME_esr)]
	str x4, [x19, #(IFRAME_far)]
	str x5, [x19, #(IFRAME_tpidr)]
.endm

.macro EXCEPTION_RETURN el
	// x19 is callee-saved so it still points to IFRAME
	// x0, x1, x18, x19 will be restored at the very end

	add x0, x19, #(IFRAME_fpu)
.if \el == 0
	// EL0 return: reload the wide SVE state from this thread's off-stack buffer.
	bl	_fp_restore_el0
.else
	bl	_fp_restore
.endif

	ldr x0,  [x19, #(IFRAME_elr)]
	ldr x1,  [x19, #(IFRAME_spsr)]
	ldr x18, [x19, #(IFRAME_sp)]

	// x0 and x1 will be restored later
	ldp	    x2,  x3, [x19, #(IFRAME_x + 2 * 8)]
	ldp	    x4,  x5, [x19, #(IFRAME_x + 4 * 8)]
	ldp	    x6,  x7, [x19, #(IFRAME_x + 6 * 8)]
	ldp	    x8,  x9, [x19, #(IFRAME_x + 8 * 8)]
	ldp	   x10, x11, [x19, #(IFRAME_x + 10 * 8)]
	ldp	   x12, x13, [x19, #(IFRAME_x + 12 * 8)]
	ldp	   x14, x15, [x19, #(IFRAME_x + 14 * 8)]
	ldp	   x16, x17, [x19, #(IFRAME_x + 16 * 8)]
	// x18, x19, x20 will be restored later
	ldp	   x21, x22, [x19, #(IFRAME_x + 21 * 8)]
	ldp	   x23, x24, [x19, #(IFRAME_x + 23 * 8)]
	ldp	   x25, x26, [x19, #(IFRAME_x + 25 * 8)]
	ldp	   x27, x28, [x19, #(IFRAME_x + 27 * 8)]
	ldp	   x29, x30, [x19, #(IFRAME_x + 29 * 8)]

	// disable interrupts before restoring ELR/SPSR/sp
	msr DAIFSet, #0xf

	msr ELR_EL1, x0
	msr SPSR_EL1, x1

.if \el == 0
	// load stack pointer for EL0 from IFRAME
	msr SP_EL0, x18

	mov sp, x20
.else
	// we stored original pointer to IFRAME, no need to unwind again there
	mov sp, x18
.endif

	ldr x0, [x19, #(IFRAME_tpidr)]
	msr TPIDR_EL0, x0

	// finally restore remaining registers
	ldp x0,   x1, [x19, #(IFRAME_x + 0 * 8)]
	ldr x20, [x19, #(IFRAME_x + 20 * 8)]
	ldp x18, x19, [x19, #(IFRAME_x + 18 * 8)]

	eret
.endm

.macro EXCEPTION_HANDLER el name func
	STATIC_FUNCTION(handle_\name):
		EXCEPTION_ENTRY \el

		// prepare aligned sp for C function
		and sp, x19, #0xfffffffffffffff0

		// call C handler, passing IFRAME in x0
		// handler can enable interrupts if it wants to
		mov x0, x19
		mov x29, x0
		bl \func

		EXCEPTION_RETURN \el
	FUNCTION_END(handle_\name)
.endm

.macro	vector	name
	.align 7
	b	handle_\name
.endm

.macro	vempty
	.align 7
	brk	0xfff
	1: b	1b
.endm

.align 11
.globl _exception_vectors
_exception_vectors:
	vempty             /* Synchronous EL1t */
	vempty             /* IRQ EL1t */
	vempty             /* FIQ EL1t */
	vempty             /* Error EL1t */

	vector el1h_sync   /* Synchronous EL1h */
	vector el1h_irq    /* IRQ EL1h */
	vector el1h_fiq    /* FIQ EL1h */
	vector el1h_error  /* Error EL1h */

	vector el0_sync    /* Synchronous 64-bit EL0 */
	vector el0_irq     /* IRQ 64-bit EL0 */
	vector el0_fiq     /* FIQ 64-bit EL0 */
	vector el0_error   /* Error 64-bit EL0 */

	vempty             /* Synchronous 32-bit EL0 */
	vempty             /* IRQ 32-bit EL0 */
	vempty             /* FIQ 32-bit EL0 */
	vempty             /* Error 32-bit EL0 */

EXCEPTION_HANDLER 1 el1h_sync do_sync_handler
EXCEPTION_HANDLER 1 el1h_irq do_irq_handler
EXCEPTION_HANDLER 1 el1h_fiq do_fiq_handler
EXCEPTION_HANDLER 1 el1h_error do_error_handler

EXCEPTION_HANDLER 0 el0_sync do_sync_handler
EXCEPTION_HANDLER 0 el0_irq do_irq_handler
EXCEPTION_HANDLER 0 el0_fiq do_fiq_handler
EXCEPTION_HANDLER 0 el0_error do_error_handler

FUNCTION(_eret_with_iframe):
	mov x19, x0
	mov x20, x1
	EXCEPTION_RETURN 0
FUNCTION_END(_eret_with_iframe)


FUNCTION(_fp_save):
	stp q0, q1, [x0], #32
	stp q2, q3, [x0], #32
	stp q4, q5, [x0], #32
	stp q6, q7, [x0], #32
	stp q8, q9, [x0], #32
	stp q10, q11, [x0], #32
	stp q12, q13, [x0], #32
	stp q14, q15, [x0], #32
	stp q16, q17, [x0], #32
	stp q18, q19, [x0], #32
	stp q20, q21, [x0], #32
	stp q22, q23, [x0], #32
	stp q24, q25, [x0], #32
	stp q26, q27, [x0], #32
	stp q28, q29, [x0], #32
	stp q30, q31, [x0], #32
	mrs x1, FPSR
	mrs x2, FPCR
	str x1, [x0], #8
	str x2, [x0], #8

	// reset FPCR and FPSR to prevent userspace state affecting kernel
	msr FPSR, xzr
	cmp x2, xzr
	beq 1f
	msr FPCR, xzr
1:
	ret
FUNCTION_END(_fp_save)

FUNCTION(_fp_restore):
	ldp q0, q1, [x0], #32
	ldp q2, q3, [x0], #32
	ldp q4, q5, [x0], #32
	ldp q6, q7, [x0], #32
	ldp q8, q9, [x0], #32
	ldp q10, q11, [x0], #32
	ldp q12, q13, [x0], #32
	ldp q14, q15, [x0], #32
	ldp q16, q17, [x0], #32
	ldp q18, q19, [x0], #32
	ldp q20, q21, [x0], #32
	ldp q22, q23, [x0], #32
	ldp q24, q25, [x0], #32
	ldp q26, q27, [x0], #32
	ldp q28, q29, [x0], #32
	ldp q30, q31, [x0], #32

	ldr x1, [x0], #8
	msr FPSR, x1

	// avoid restoring FPCR if it hasn't changed
	ldr x2, [x0], #8
	mrs x3, FPCR
	cmp x3, x2
	beq 1f
	msr FPCR, x2
1:
	ret
FUNCTION_END(_fp_restore)


// void _fp_save_el0(aarch64_fpu_state* fpu /* x0 = &iframe.fpu */)
//
// EL0 exception entry. Saves the NEON view (V0-31 + FPSR/FPCR, and scrubs
// FPSR/FPCR) into the iframe exactly as the EL1 path does, then -- on SVE
// hardware -- also spills the full Z0-31/P0-15/FFR into the current thread's
// off-stack buffer (TPIDR_EL1 + THREAD_sve). Called with GP registers already
// spilled to the iframe, so x1-x15 are free scratch. Keeping the wide state off
// the kernel stack is what makes this safe against the stack overflow that held
// the first attempt.
FUNCTION(_fp_save_el0):
	mov x14, lr			// _fp_save is reached via bl; preserve our return addr
	bl	_fp_save		// V0-31 + FPSR/FPCR -> iframe; FPSR/FPCR scrubbed
	mov lr, x14

	adrp x9, gArm64SVEVectorBytes
	ldr w9, [x9, #:lo12:gArm64SVEVectorBytes]
	cbz w9, 1f

	// Current thread's off-stack SVE buffer. THREAD_sve can exceed the 12-bit
	// add immediate, so materialise it in a register first.
	mrs x8, TPIDR_EL1
	mov x9, #(THREAD_sve & 0xffff)
	movk x9, #((THREAD_sve >> 16) & 0xffff), lsl #16
	add x8, x8, x9

	// The "MUL VL" stride is the effective VL, which ZCR_EL1 clamped to at most
	// SVE_MAX_VL_BYTES, so it fits the fixed buffer. stp q above did not disturb
	// the SVE upper bits, so these Zn reads still hold the interrupted state.
	add x10, x8, #SVE_z
	str z0,  [x10, #0, MUL VL]
	str z1,  [x10, #1, MUL VL]
	str z2,  [x10, #2, MUL VL]
	str z3,  [x10, #3, MUL VL]
	str z4,  [x10, #4, MUL VL]
	str z5,  [x10, #5, MUL VL]
	str z6,  [x10, #6, MUL VL]
	str z7,  [x10, #7, MUL VL]
	str z8,  [x10, #8, MUL VL]
	str z9,  [x10, #9, MUL VL]
	str z10, [x10, #10, MUL VL]
	str z11, [x10, #11, MUL VL]
	str z12, [x10, #12, MUL VL]
	str z13, [x10, #13, MUL VL]
	str z14, [x10, #14, MUL VL]
	str z15, [x10, #15, MUL VL]
	str z16, [x10, #16, MUL VL]
	str z17, [x10, #17, MUL VL]
	str z18, [x10, #18, MUL VL]
	str z19, [x10, #19, MUL VL]
	str z20, [x10, #20, MUL VL]
	str z21, [x10, #21, MUL VL]
	str z22, [x10, #22, MUL VL]
	str z23, [x10, #23, MUL VL]
	str z24, [x10, #24, MUL VL]
	str z25, [x10, #25, MUL VL]
	str z26, [x10, #26, MUL VL]
	str z27, [x10, #27, MUL VL]
	str z28, [x10, #28, MUL VL]
	str z29, [x10, #29, MUL VL]
	str z30, [x10, #30, MUL VL]
	str z31, [x10, #31, MUL VL]

	add x11, x8, #SVE_p
	str p0,  [x11, #0, MUL VL]
	str p1,  [x11, #1, MUL VL]
	str p2,  [x11, #2, MUL VL]
	str p3,  [x11, #3, MUL VL]
	str p4,  [x11, #4, MUL VL]
	str p5,  [x11, #5, MUL VL]
	str p6,  [x11, #6, MUL VL]
	str p7,  [x11, #7, MUL VL]
	str p8,  [x11, #8, MUL VL]
	str p9,  [x11, #9, MUL VL]
	str p10, [x11, #10, MUL VL]
	str p11, [x11, #11, MUL VL]
	str p12, [x11, #12, MUL VL]
	str p13, [x11, #13, MUL VL]
	str p14, [x11, #14, MUL VL]
	str p15, [x11, #15, MUL VL]

	// FFR is read through a predicate. p0 has already been stored, so it is free
	// to reuse as scratch here.
	add x12, x8, #SVE_ffr
	rdffr p0.b
	str p0, [x12, #0, MUL VL]
1:
	ret
FUNCTION_END(_fp_save_el0)


// void _fp_restore_el0(aarch64_fpu_state* fpu /* x0 = &iframe.fpu */)
//
// EL0 exception return. On SVE hardware Z0-31 are restored from this thread's
// off-stack buffer and are the single source of truth for V0-31 (a trailing
// ldr q would zero the SVE upper bits, so the iframe V view is deliberately NOT
// reloaded here); FPSR/FPCR come from the iframe. Any fp_q edits a signal
// handler made are folded into the Z low lanes by arch_restore_signal_frame()
// before we get here. On non-SVE hardware this tail-calls the plain V restore.
FUNCTION(_fp_restore_el0):
	adrp x9, gArm64SVEVectorBytes
	ldr w9, [x9, #:lo12:gArm64SVEVectorBytes]
	cbz w9, 2f

	mov x13, x0			// keep &iframe.fpu for the FPSR/FPCR reload below

	mrs x8, TPIDR_EL1
	mov x9, #(THREAD_sve & 0xffff)
	movk x9, #((THREAD_sve >> 16) & 0xffff), lsl #16
	add x8, x8, x9

	// FFR first, using p0 as scratch, before the real predicates load.
	add x12, x8, #SVE_ffr
	ldr p0, [x12, #0, MUL VL]
	wrffr p0.b

	add x11, x8, #SVE_p
	ldr p0,  [x11, #0, MUL VL]
	ldr p1,  [x11, #1, MUL VL]
	ldr p2,  [x11, #2, MUL VL]
	ldr p3,  [x11, #3, MUL VL]
	ldr p4,  [x11, #4, MUL VL]
	ldr p5,  [x11, #5, MUL VL]
	ldr p6,  [x11, #6, MUL VL]
	ldr p7,  [x11, #7, MUL VL]
	ldr p8,  [x11, #8, MUL VL]
	ldr p9,  [x11, #9, MUL VL]
	ldr p10, [x11, #10, MUL VL]
	ldr p11, [x11, #11, MUL VL]
	ldr p12, [x11, #12, MUL VL]
	ldr p13, [x11, #13, MUL VL]
	ldr p14, [x11, #14, MUL VL]
	ldr p15, [x11, #15, MUL VL]

	add x10, x8, #SVE_z
	ldr z0,  [x10, #0, MUL VL]
	ldr z1,  [x10, #1, MUL VL]
	ldr z2,  [x10, #2, MUL VL]
	ldr z3,  [x10, #3, MUL VL]
	ldr z4,  [x10, #4, MUL VL]
	ldr z5,  [x10, #5, MUL VL]
	ldr z6,  [x10, #6, MUL VL]
	ldr z7,  [x10, #7, MUL VL]
	ldr z8,  [x10, #8, MUL VL]
	ldr z9,  [x10, #9, MUL VL]
	ldr z10, [x10, #10, MUL VL]
	ldr z11, [x10, #11, MUL VL]
	ldr z12, [x10, #12, MUL VL]
	ldr z13, [x10, #13, MUL VL]
	ldr z14, [x10, #14, MUL VL]
	ldr z15, [x10, #15, MUL VL]
	ldr z16, [x10, #16, MUL VL]
	ldr z17, [x10, #17, MUL VL]
	ldr z18, [x10, #18, MUL VL]
	ldr z19, [x10, #19, MUL VL]
	ldr z20, [x10, #20, MUL VL]
	ldr z21, [x10, #21, MUL VL]
	ldr z22, [x10, #22, MUL VL]
	ldr z23, [x10, #23, MUL VL]
	ldr z24, [x10, #24, MUL VL]
	ldr z25, [x10, #25, MUL VL]
	ldr z26, [x10, #26, MUL VL]
	ldr z27, [x10, #27, MUL VL]
	ldr z28, [x10, #28, MUL VL]
	ldr z29, [x10, #29, MUL VL]
	ldr z30, [x10, #30, MUL VL]
	ldr z31, [x10, #31, MUL VL]

	// FPSR/FPCR from the iframe (the SVE buffer does not carry them).
	ldr x1, [x13, #FPU_fpsr]
	msr FPSR, x1
	ldr x2, [x13, #FPU_fpcr]
	mrs x3, FPCR
	cmp x3, x2
	beq 1f
	msr FPCR, x2
1:
	ret

2:
	// Non-SVE hardware: V0-31 + FPSR/FPCR from the iframe. x0 is untouched on
	// this path, so tail-call the shared NEON restore.
	b _fp_restore
FUNCTION_END(_fp_restore_el0)


FUNCTION(_arch_context_swap):
	// save
	stp x19, x20, [x0], #16
	stp x21, x22, [x0], #16
	stp x23, x24, [x0], #16
	stp x25, x26, [x0], #16
	stp x27, x28, [x0], #16
	stp x29, x30, [x0], #16

	mov x2, sp
	str x2, [x0], #8

	stp  d8,  d9, [x0], #16
	stp d10, d11, [x0], #16
	stp d12, d13, [x0], #16
	stp d14, d15, [x0], #16

	// restore
	ldp x19, x20, [x1], #16
	ldp x21, x22, [x1], #16
	ldp x23, x24, [x1], #16
	ldp x25, x26, [x1], #16
	ldp x27, x28, [x1], #16
	ldp x29, x30, [x1], #16

	ldr x2, [x1], #8
	mov sp, x2

	ldp  d8,  d9, [x1], #16
	ldp d10, d11, [x1], #16
	ldp d12, d13, [x1], #16
	ldp d14, d15, [x1], #16

	// pass x29 as argument to thread entry function
	mov x0, x29
	ret
FUNCTION_END(_arch_context_swap)

/*!	\fn void arch_debug_call_with_fault_handler(cpu_ent* cpu,
		jmp_buf jumpBuffer, void (*function)(void*), void* parameter)

	Called by debug_call_with_fault_handler() to do the dirty work of setting
	the fault handler and calling the function. If the function causes a page
	fault, the arch_debug_call_with_fault_handler() calls longjmp() with the
	given \a jumpBuffer. Otherwise it returns normally.

	debug_call_with_fault_handler() has already saved the CPU's fault_handler
	and fault_handler_stack_pointer and will reset them later, so
	arch_debug_call_with_fault_handler() doesn't need to care about it.

	\param cpu The \c cpu_ent for the current CPU.
	\param jumpBuffer Buffer to be used for longjmp().
	\param function The function to be called.
	\param parameter The parameter to be passed to the function to be called.
*/
FUNCTION(arch_debug_call_with_fault_handler):
	adrp x4, fault
	add x4, x4, :lo12:fault
	str x4, [x0, #CPU_ENT_fault_handler]
	str x1, [x0, #CPU_ENT_fault_handler_stack_pointer]

	mov x0, x3
	br x2

fault:
	mov x0, sp
	mov x1, #1
	b longjmp
FUNCTION_END(arch_debug_call_with_fault_handler)


/* addr_t arm64_get_fp(void) */
FUNCTION(arm64_get_fp):
	mov x0, x29
	ret
FUNCTION_END(arm64_get_fp)
