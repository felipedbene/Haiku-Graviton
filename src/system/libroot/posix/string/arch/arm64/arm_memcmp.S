/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	DeBeOS glue for the vendored Arm optimized-routines memcmp.

	The upstream source is #included verbatim (it defines the internal
	__memcmp_aarch64) and the public C symbol is aliased to it below. The alias
	has to be emitted in the same translation unit as the definition: a `.set`
	from a separate object to an as-yet-undefined symbol does not produce an
	alias (verified with this tree's aarch64 ld). Each vendored routine gets its
	own glue unit because they reuse the same local labels (.Lloop, .Ltail, ...)
	and so cannot share a translation unit. */


/* Suppress the BTI/PAC .note.gnu.property section the vendored asmdefs.h emits
   by default; no other libroot/kernel object carries it. Set here rather than
   as a build flag because Haiku's As rule assembles .S with ASFLAGS, not the
   CCFLAGS a SubDir*Flags -D would land in, so a Jamfile define never reaches
   the assembler. */
#define WANT_GNU_PROPERTY 0

#include "arm-optimized-routines/memcmp.S"

.global memcmp
.type memcmp, %function
.set memcmp, __memcmp_aarch64
