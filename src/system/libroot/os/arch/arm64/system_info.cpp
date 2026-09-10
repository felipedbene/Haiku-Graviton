/*
 * Copyright 2026 DeBeOS. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/auxv.h>

#include <errno.h>

#include <OS.h>

#include <commpage_defs.h>
#include <errno_private.h>


// Set by the runtime loader; the base of the shared commpage mapped into every
// team. The kernel publishes the CPU feature words there (see the arm64
// arch_commpage.cpp), because EL0 cannot read the ID_AA64* registers itself.
extern const void* __gCommPageAddress;


unsigned long
getauxval(unsigned long type)
{
	addr_t commPageTable = (addr_t)__gCommPageAddress;

	switch (type) {
		case AT_HWCAP:
		case AT_HWCAP2:
		{
			addr_t offset
				= ((addr_t*)commPageTable)[COMMPAGE_ENTRY_ARM64_HWCAP];
			// A zero offset means the running kernel did not publish the entry;
			// treat it as "unknown" rather than dereferencing the commpage
			// header that lives at offset 0.
			if (offset == 0)
				break;

			struct arm64_commpage_hwcap* caps
				= (struct arm64_commpage_hwcap*)(offset + commPageTable);
			return type == AT_HWCAP ? caps->hwcap : caps->hwcap2;
		}

		case AT_PAGESZ:
			return B_PAGE_SIZE;

		default:
			break;
	}

	// Match glibc: unsupported requests report ENOENT and return 0.
	__set_errno(ENOENT);
	return 0;
}
