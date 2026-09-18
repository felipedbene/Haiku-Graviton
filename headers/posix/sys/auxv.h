/*
 * Copyright 2026 DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H


#include <sys/cdefs.h>


/* ELF auxiliary vector types.
 *
 * DeBeOS does not build a Linux/glibc-style auxiliary vector on the initial
 * user stack; getauxval() is served from a kernel-published word (see the
 * arm64 commpage HWCAP entry). The type numbers below match the well-known
 * ELF/glibc values so that ported software keying off AT_HWCAP continues to
 * work unchanged. Only the entries getauxval() can actually answer are
 * meaningful here; the rest are provided for source compatibility.
 */
#define AT_NULL			0	/* end of vector */
#define AT_IGNORE		1	/* entry should be ignored */
#define AT_EXECFD		2	/* file descriptor of program */
#define AT_PHDR			3	/* program headers for program */
#define AT_PHENT		4	/* size of program header entry */
#define AT_PHNUM		5	/* number of program headers */
#define AT_PAGESZ		6	/* system page size */
#define AT_BASE			7	/* base address of interpreter */
#define AT_FLAGS		8	/* flags */
#define AT_ENTRY		9	/* entry point of program */
#define AT_NOTELF		10	/* program is not ELF */
#define AT_UID			11	/* real uid */
#define AT_EUID			12	/* effective uid */
#define AT_GID			13	/* real gid */
#define AT_EGID			14	/* effective gid */
#define AT_PLATFORM		15	/* string identifying platform */
#define AT_HWCAP		16	/* machine-dependent hints about the processor */
#define AT_CLKTCK		17	/* frequency of times() */
#define AT_SECURE		23	/* boolean, was exec setuid-like? */
#define AT_RANDOM		25	/* address of 16 random bytes */
#define AT_HWCAP2		26	/* extension of AT_HWCAP */
#define AT_EXECFN		31	/* filename of program */


#if defined(__aarch64__) || defined(__arm64__)

/* AT_HWCAP bits (arm64), matching the Linux/glibc layout so that existing
 * feature-probing code in ports keeps working unchanged. Populated by the
 * kernel from ID_AA64ISAR0/1_EL1 and ID_AA64PFR0_EL1 (see the arm64
 * arch_cpu.cpp).
 */
#define HWCAP_FP		(1UL << 0)
#define HWCAP_ASIMD		(1UL << 1)
#define HWCAP_EVTSTRM	(1UL << 2)
#define HWCAP_AES		(1UL << 3)
#define HWCAP_PMULL		(1UL << 4)
#define HWCAP_SHA1		(1UL << 5)
#define HWCAP_SHA2		(1UL << 6)
#define HWCAP_CRC32		(1UL << 7)
#define HWCAP_ATOMICS	(1UL << 8)
#define HWCAP_FPHP		(1UL << 9)
#define HWCAP_ASIMDHP	(1UL << 10)
#define HWCAP_CPUID		(1UL << 11)
#define HWCAP_ASIMDRDM	(1UL << 12)
#define HWCAP_JSCVT		(1UL << 13)
#define HWCAP_FCMA		(1UL << 14)
#define HWCAP_LRCPC		(1UL << 15)
#define HWCAP_DCPOP		(1UL << 16)
#define HWCAP_SHA3		(1UL << 17)
#define HWCAP_SM3		(1UL << 18)
#define HWCAP_SM4		(1UL << 19)
#define HWCAP_ASIMDDP	(1UL << 20)
#define HWCAP_SHA512	(1UL << 21)
#define HWCAP_SVE		(1UL << 22)
#define HWCAP_ASIMDFHM	(1UL << 23)
#define HWCAP_DIT		(1UL << 24)
#define HWCAP_USCAT		(1UL << 25)
#define HWCAP_ILRCPC	(1UL << 26)
#define HWCAP_FLAGM		(1UL << 27)
#define HWCAP_SSBS		(1UL << 28)
#define HWCAP_SB		(1UL << 29)
#define HWCAP_PACA		(1UL << 30)
#define HWCAP_PACG		(1UL << 31)

/* AT_HWCAP2 bits (arm64), matching the Linux/glibc layout. */
#define HWCAP2_DCPODP		(1UL << 0)
#define HWCAP2_SVE2			(1UL << 1)
#define HWCAP2_SVEAES		(1UL << 2)
#define HWCAP2_SVEPMULL		(1UL << 3)
#define HWCAP2_SVEBITPERM	(1UL << 4)
#define HWCAP2_SVESHA3		(1UL << 5)
#define HWCAP2_SVESM4		(1UL << 6)
#define HWCAP2_FLAGM2		(1UL << 7)
#define HWCAP2_FRINT		(1UL << 8)
#define HWCAP2_SVEI8MM		(1UL << 9)
#define HWCAP2_SVEF32MM		(1UL << 10)
#define HWCAP2_SVEF64MM		(1UL << 11)
#define HWCAP2_SVEBF16		(1UL << 12)
#define HWCAP2_I8MM			(1UL << 13)
#define HWCAP2_BF16			(1UL << 14)

#endif	/* __aarch64__ */


__BEGIN_DECLS

unsigned long getauxval(unsigned long type);

__END_DECLS


#endif	/* _SYS_AUXV_H */
