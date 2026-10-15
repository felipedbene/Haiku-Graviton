/*
 * Copyright 2018-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */
#ifndef _FBSD_COMPAT_MACHINE_BUS_H_
#define _FBSD_COMPAT_MACHINE_BUS_H_

#include <machine/_bus.h>
#include <machine/cpufunc.h>

#if defined(__i386__) || defined(__amd64__)
#  include <machine/x86/bus.h>
// arm64 takes the generic (MMIO-only) accessors for the same reason riscv64
// does: there is no I/O-port space on the architecture at all, so a BAR that
// claims to be one cannot be addressed and the generic header's
// BUS_SPACE_TAG_IO rejection is the honest answer rather than a silent alias
// onto whatever lives at port 0 (see #460).
#elif (defined(__riscv) && __riscv_xlen == 64) \
	|| defined(__aarch64__) || defined(__arm64__)
#  include <machine/generic/bus.h>
#else
#  error Need a bus.h for this arch!
#endif

#endif /* _FBSD_COMPAT_MACHINE_BUS_H_ */
