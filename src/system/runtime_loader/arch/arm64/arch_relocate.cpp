/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch_elf.h"
#include "runtime_loader_private.h"

#include <commpage_defs.h>
#include <runtime_loader.h>


//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


// STT_GNU_IFUNC resolver ABI (AArch64). glibc calls the resolver with the
// AT_HWCAP word in x0 (OR'd with _IFUNC_ARG_HWCAP so newer resolvers know the
// second argument is present) and a pointer to an __ifunc_arg_t in x1. Older
// resolvers ignore x1 and just read the HWCAP bits from x0; either way the
// resolver returns the address of the implementation to use. These definitions
// mirror <sys/ifunc.h>, which DeBeOS does not ship.
#define _IFUNC_ARG_HWCAP (1ULL << 62)

typedef struct __ifunc_arg_t {
	unsigned long	_size;		// sizeof (__ifunc_arg_t)
	unsigned long	_hwcap;		// AT_HWCAP
	unsigned long	_hwcap2;	// AT_HWCAP2
} __ifunc_arg_t;


// Read the CPU feature words the kernel publishes in the commpage. This mirrors
// libroot's getauxval(AT_HWCAP) read (src/system/libroot/os/arch/arm64/
// system_info.cpp) -- keep the two in sync if the commpage HWCAP layout ever
// changes. EL0 cannot read the ID_AA64* registers itself, so the commpage is
// the only place the resolver can learn what the hardware supports.
static void
arch_get_hwcap(uint64* hwcap, uint64* hwcap2)
{
	*hwcap = 0;
	*hwcap2 = 0;

	addr_t commPageTable = (addr_t)__gCommPageAddress;
	if (commPageTable == 0)
		return;

	addr_t offset = ((addr_t*)commPageTable)[COMMPAGE_ENTRY_ARM64_HWCAP];
	// A zero offset means the running kernel did not publish the entry; leave
	// the caps at zero rather than dereferencing the commpage header at 0.
	if (offset == 0)
		return;

	struct arm64_commpage_hwcap* caps
		= (struct arm64_commpage_hwcap*)(offset + commPageTable);
	*hwcap = caps->hwcap;
	*hwcap2 = caps->hwcap2;
}


// Invoke an STT_GNU_IFUNC resolver and return the implementation address it
// selects. resolverAddress is the resolver's runtime address. Called by the
// runtime loader (resolve_deferred_ifuncs / dlsym) only after remap_images()
// has made the resolver's text executable again. Declared in
// runtime_loader_private.h.
addr_t
arch_call_ifunc_resolver(addr_t resolverAddress)
{
	uint64 hwcap;
	uint64 hwcap2;
	arch_get_hwcap(&hwcap, &hwcap2);

	__ifunc_arg_t arg;
	arg._size = sizeof(arg);
	arg._hwcap = hwcap;
	arg._hwcap2 = hwcap2;

	typedef addr_t (*ifunc_resolver_t)(uint64, __ifunc_arg_t*);
	ifunc_resolver_t resolver = (ifunc_resolver_t)resolverAddress;
	return resolver(hwcap | _IFUNC_ARG_HWCAP, &arg);
}


static status_t
relocate_rela(image_t* rootImage, image_t* image, Elf64_Rela* rel,
	size_t relLength, SymbolLookupCache* cache)
{
	for (size_t i = 0; i < relLength / sizeof(Elf64_Rela); i++) {
		int type = ELF64_R_TYPE(rel[i].r_info);
		int symIndex = ELF64_R_SYM(rel[i].r_info);
		Elf64_Addr symAddr = 0;
		image_t* symbolImage = NULL;
		bool symIsIndirect = false;

		// Resolve the symbol, if any.
		if (symIndex != 0) {
			Elf64_Sym* sym = SYMBOL(image, symIndex);

			status_t status = resolve_symbol(rootImage, image, sym, cache,
				&symAddr, &symbolImage, &symIsIndirect);
			if (status != B_OK) {
				TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status));
				printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status);
				return status;
			}
		}

		// Address of the relocation.
		Elf64_Addr relocAddr = image->regions[0].delta + rel[i].r_offset;

		// Calculate the relocation value.
		Elf64_Addr relocValue;
		// For an STT_GNU_IFUNC relocation, the final value is the resolver's
		// result plus the addend; ifuncResolver is the resolver address, or 0
		// if this is not an ifunc relocation.
		Elf64_Addr ifuncResolver = 0;
		switch (type) {
			case R_AARCH64_NONE:
				continue;
			case R_AARCH64_ABS64:
			case R_AARCH64_GLOB_DAT:
			case R_AARCH64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				// For an ifunc definition symAddr is the resolver address; the
				// implementation is resolver() and the addend is applied to it.
				if (symIsIndirect)
					ifuncResolver = symAddr;
				break;
			case R_AARCH64_RELATIVE:
				relocValue = image->regions[0].delta + rel[i].r_addend;
				break;
			case R_AARCH64_IRELATIVE:
				// The addend is the resolver's link-time address; the slot must
				// end up holding the resolver's result (no further addend).
				relocValue = image->regions[0].delta + rel[i].r_addend;
				ifuncResolver = relocValue;
				break;
			case R_AARCH64_TLS_DTPMOD64:
				relocValue = symbolImage == NULL
							? image->dso_tls_id : symbolImage->dso_tls_id;
				break;
			case R_AARCH64_TLS_DTPREL64:
				relocValue = symAddr;
				break;
			default:
				TRACE(("unhandled relocation type %d\n", type));
				return B_BAD_DATA;
		}

		*(Elf64_Addr *)relocAddr = relocValue;

		// Record ifunc relocations; the loader invokes the resolver and writes
		// resolver() + addend into the slot after remap_images() has made text
		// executable (see resolve_deferred_ifuncs()). For IRELATIVE the addend
		// is already folded into the resolver address, so no addend is re-added.
		if (ifuncResolver != 0) {
			Elf64_Addr ifuncAddend
				= (type == R_AARCH64_IRELATIVE) ? 0 : rel[i].r_addend;
			defer_ifunc_relocation((addr_t*)relocAddr, ifuncResolver,
				ifuncAddend);
		}
	}

	return B_OK;
}


status_t
arch_relocate_image(image_t *rootImage, image_t *image,
	SymbolLookupCache* cache)
{
	status_t status;

	// No REL on arm64.

	// Perform RELA relocations.
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	// PLT relocations (they are RELA on arm64).
	if (image->pltrel) {
		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}
