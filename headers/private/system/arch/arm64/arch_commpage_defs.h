/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H
#define _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H

#ifndef _SYSTEM_COMMPAGE_DEFS_H
#	error Must not be included directly. Include <commpage_defs.h> instead!
#endif

#define COMMPAGE_ENTRY_ARM64_THREAD_EXIT (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 0)
#define COMMPAGE_ENTRY_ARM64_SIGNAL_HANDLER (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 1)

// CPU feature bitmaps in the Linux/glibc AT_HWCAP layout. EL0 cannot read the
// ID_AA64* feature registers directly (and DeBeOS does not trap-and-emulate the
// MRS), so the kernel computes these once from EL1 and publishes them here for
// getauxval() in libroot.
#define COMMPAGE_ENTRY_ARM64_HWCAP (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 2)

// This header is included from assembly, so keep the struct out of that path
// and avoid depending on Haiku's typedefs (unsigned long long is 64-bit on
// every arm64 target and needs no include).
#ifndef __ASSEMBLER__
struct arm64_commpage_hwcap {
	unsigned long long	hwcap;		// answers getauxval(AT_HWCAP)
	unsigned long long	hwcap2;		// answers getauxval(AT_HWCAP2)
};
#endif

#endif	/* _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H */
