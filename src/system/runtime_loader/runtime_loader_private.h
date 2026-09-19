/*
 * Copyright 2003-2011, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2002, Manuel J. Petit. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */
#ifndef RUNTIME_LOADER_PRIVATE_H
#define RUNTIME_LOADER_PRIVATE_H

#include <user_runtime.h>
#include <runtime_loader.h>

#include "tracing_config.h"

#include "utility.h"


// STT_GNU_IFUNC (== STT_LOOS) marks a symbol whose runtime address is produced
// by calling it as a resolver (GNU indirect functions / ifunc). Not defined in
// <elf.h>, so keep a private copy here for the loader's ifunc handling.
#ifndef STT_GNU_IFUNC
#	define STT_GNU_IFUNC 10
#endif

// Arches whose runtime_loader implements STT_GNU_IFUNC (ifunc) resolution
// define RLD_IFUNC_SUPPORTED and an arch_call_ifunc_resolver(). All ifunc
// handling in the shared loader code is compiled only for those arches, so the
// others build and behave exactly as before.
#if defined(__aarch64__)
#	define RLD_IFUNC_SUPPORTED 1
#endif

#ifdef RLD_IFUNC_SUPPORTED
// Invoke an STT_GNU_IFUNC resolver (arch-specific ABI, defined per arch) and
// return the implementation address it selects. Only safe to call once the
// resolver's text segment is executable (after remap_images()).
addr_t arch_call_ifunc_resolver(addr_t resolverAddress);
#endif


//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


#if RUNTIME_LOADER_TRACING
#	define KTRACE(x...)	ktrace_printf(x)
#else
#	define KTRACE(x...)
#endif	// RUNTIME_LOADER_TRACING

#if defined(_COMPAT_MODE) && !defined(__x86_64__)
	#if __GNUC__ == 2
		#define RLD_PREFIX "runtime_loader_x86_gcc2: "
	#else
		#define RLD_PREFIX "runtime_loader_x86: "
	#endif
#endif
#ifndef RLD_PREFIX
#define RLD_PREFIX "runtime_loader: "
#endif
#define FATAL(x...)							\
	do {									\
		dprintf(RLD_PREFIX x);		\
		if (!gProgramLoaded)				\
			printf(RLD_PREFIX x);	\
	} while (false)


struct SymbolLookupCache;


extern struct user_space_program_args* gProgramArgs;
extern void* __gCommPageAddress;
extern struct rld_export gRuntimeLoader;
extern char* (*gGetEnv)(const char* name);
extern bool gProgramLoaded;
extern image_t* gProgramImage;


extern "C" {

int runtime_loader(void* arg, void* commpage);
int open_executable(char* name, image_type type, const char* rpath,
	const char* runpath, const char* programPath, const char* requestingObjectPath,
	const char* abiSpecificSubDir);
status_t test_executable(const char* path, char* interpreter);
status_t get_executable_architecture(const char* path,
	const char** _architecture);

void terminate_program(void);
image_id load_program(char const* path, void** entry);
image_id load_library(char const* path, uint32 flags, bool addOn,
	void* caller, void** _handle);
status_t unload_library(void* handle, image_id imageID, bool addOn);
status_t get_nth_symbol(image_id imageID, int32 num, char* nameBuffer,
	int32* _nameLength, int32* _type, void** _location);
status_t get_nearest_symbol_at_address(void* address, image_id* _imageID,
	char** _imagePath, char** _imageName, char** _symbolName, int32* _type,
	void** _location, bool* _exactMatch);
status_t get_symbol(image_id imageID, char const* symbolName, int32 symbolType,
	bool recursive, image_id* _inImage, void** _location);
status_t get_library_symbol(void* handle, void* caller, const char* symbolName,
	void** _location);
status_t get_next_image_dependency(image_id id, uint32* cookie,
	const char** _name);
// If \a _isIndirect is non-NULL, ifunc handling is enabled: an STT_GNU_IFUNC
// definition is accepted (its type no longer conflicts with a plain function
// reference), *_isIndirect is set true and \a sym_addr is returned as the
// resolver's address (not invoked -- the caller defers the call until text is
// executable). Arches without an ifunc resolver ABI pass NULL and see the
// pre-ifunc behavior.
int resolve_symbol(image_t* rootImage, image_t* image, elf_sym* sym,
	SymbolLookupCache* cache, addr_t* sym_addr, image_t** symbolImage = NULL,
	bool* _isIndirect = NULL);

#ifdef RLD_IFUNC_SUPPORTED
// Record an STT_GNU_IFUNC relocation to be completed once text is executable
// again: the slot is set to arch_call_ifunc_resolver(resolverAddress) + addend
// by resolve_deferred_ifuncs().
void defer_ifunc_relocation(addr_t* slot, addr_t resolverAddress, addr_t addend);
#endif


status_t elf_verify_header(void* header, size_t length);
#ifdef _COMPAT_MODE
#ifdef __x86_64__
status_t elf32_verify_header(void *header, size_t length);
#else
status_t elf64_verify_header(void *header, size_t length);
#endif	// __x86_64__
#endif	// _COMPAT_MODE
void rldelf_init(void);
void rldexport_init(void);
void set_abi_api_version(int abi_version, int api_version);
status_t elf_reinit_after_fork();

status_t heap_init();
status_t heap_reinit_after_fork();

// arch dependent prototypes
status_t arch_relocate_image(image_t* rootImage, image_t* image,
	SymbolLookupCache* cache);

}

#endif	/* RUNTIME_LOADER_PRIVATE_H */
