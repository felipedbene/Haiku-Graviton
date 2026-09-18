/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	DeBeOS glue for the vendored Arm optimized-routines memmove.

	Upstream ships memmove and memcpy from a single source (memcpy.S): the
	__memmove_aarch64 entry falls through into the shared body, which does an
	overlap check and copies backwards when the destination is above the source.
	DeBeOS keeps its own memcpy.c as the public memcpy, so only __memmove_aarch64
	is aliased here; the __memcpy_aarch64 symbol the same file defines is left
	unused. See arm_memcmp.S for why the alias lives in this unit. */


#define WANT_GNU_PROPERTY 0	/* no BTI/PAC note section; see arm_memcmp.S */

#include "arm-optimized-routines/memcpy.S"

.global memmove
.type memmove, %function
.set memmove, __memmove_aarch64
