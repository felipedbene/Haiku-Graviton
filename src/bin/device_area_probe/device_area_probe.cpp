/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * device_area_probe - reproducer for the CACHE_TYPE_DEVICE mprotect() panic.
 *
 * _user_set_memory_protection() re-maps every present page of the range it is
 * given. For each page it asks the translation map for the physical address,
 * then calls vm_lookup_page() on it so that it can compare the page's cache
 * against the area's top cache. vm_lookup_page() only resolves addresses inside
 * the page array, i.e. RAM the page allocator owns. A CACHE_TYPE_DEVICE area
 * maps physical memory that is *not* in that array, so the lookup returns NULL
 * and the loop panics -- with a perfectly valid, non-zero physical address.
 *
 * This is a different defect from the arm64 Query() valid-bit bug (see
 * graviton/docs/arm64-mprotect-query-present.md and src/bin/mprotect_probe):
 * there the physical address was 0 because the entry was absent, and the fix
 * was to stop reporting absent entries as present. Here the mapping really is
 * present and the address really is correct; there is simply no vm_page behind
 * it, and no architecture's Query() can help.
 *
 * The probe has three arms, because the questions they answer are different.
 *
 *   Arm 1 -- UNPRIVILEGED, via a graphics driver. The framebuffer and vesa
 *     drivers answer VESA_CLONE_FRAME_BUFFER by calling vm_clone_area() with
 *     kernel = true, into B_CURRENT_TEAM, with B_READ_AREA | B_WRITE_AREA and
 *     protection_max 0. That hands the caller a CACHE_TYPE_DEVICE area in its
 *     own address space -- the cloneable and protection_max checks are both
 *     !kernel-gated, so the source area does not even have to be cloneable.
 *     Nothing on the way checks a uid: devfs implements no access() hook and
 *     the VFS grants access when there is none, so the 0644 on the device node
 *     is advisory. This is also how app_server gets at the framebuffer, so the
 *     path is certainly live wherever the driver is.
 *
 *   Arm 2 -- UNPRIVILEGED, via a cloneable device area. Some device areas are
 *     published with B_CLONEABLE_AREA, which is what vm_clone_area() requires
 *     of a cross-address-space clone from a non-kernel caller; none in this
 *     tree carries B_KERNEL_AREA, the other thing it would reject. find_area()
 *     is not access-checked at all, so the area_id can be had by name even by
 *     a team that may not enumerate the owner's areas.
 *
 *   Arm 3 -- MECHANISM ONLY, needs root. /dev/misc/poke's POKE_MAP_MEMORY maps
 *     arbitrary physical memory straight into the calling team's address space,
 *     producing a user-space CACHE_TYPE_DEVICE area on any machine that has the
 *     driver. poke_open() rejects non-root callers, so this arm says nothing
 *     about unprivileged reachability -- it exists so that the panic can be
 *     reproduced deterministically on a headless machine with no graphics
 *     hardware at all, where arms 1 and 2 have nothing to work with.
 *
 * On an unpatched kernel the first arm that manages to build a device area
 * never returns from mprotect(). On a patched kernel every attempt returns and
 * the final verdict line is printed. Output is unbuffered so that the serial
 * console keeps everything printed before a panic.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>

#include <poke.h>
#include <vesa_info.h>


#define MARKER "device_area_probe"

/*
 * Graphics devices whose VESA_CLONE_FRAME_BUFFER ioctl clones the framebuffer
 * device area into the caller's address space. Only the first one ships on
 * every architecture; "graphics/vesa" is x86-family.
 */
static const char* const kGraphicsDevices[] = {
	"/dev/graphics/framebuffer",
	"/dev/graphics/vesa",
	NULL
};

/*
 * CACHE_TYPE_DEVICE areas that this tree publishes with B_CLONEABLE_AREA set
 * and B_KERNEL_AREA clear, i.e. the ones an unprivileged clone_area() accepts.
 * "frame buffer" is the interesting one: it is created by the kernel itself
 * (src/system/kernel/debug/frame_buffer_console.cpp) on every architecture,
 * whenever the boot loader handed over a frame buffer -- but the framebuffer
 * driver replaces and deletes it when /dev/graphics/framebuffer is first
 * opened, so it is only there until then. The rest belong to the legacy x86
 * graphics drivers.
 */
static const char* const kDeviceAreaNames[] = {
	"frame buffer",
	"vesa frame buffer",
	"ATI mmio registers",
	"3DFX mmio registers",
	"i810 mmio registers",
	NULL
};

/*
 * Physical address for arm 3. It has to be outside the page array, which is
 * what makes vm_lookup_page() fail; 16 TiB is far above the RAM of any machine
 * this runs on, and the probe refuses to continue if that stops being true.
 * Nothing ever dereferences the mapping, so it does not matter that no device
 * decodes the address.
 */
static const uint64 kUnbackedPhysicalAddress = (uint64)1 << 44;
static const size_t kUnbackedSize = 4096;

/* Both directions, so that one of them is guaranteed to differ from the area's
   current protection and reach the remap loop instead of short-circuiting on an
   equal value. */
static const int kProtections[] = { PROT_READ, PROT_READ | PROT_WRITE };
#define kProtectionCount (sizeof(kProtections) / sizeof(kProtections[0]))


/*!	mprotect() the whole of \a area, which must belong to us. Returns 0 if the
	call returned at all -- on an unpatched kernel it does not return. */
static int
mprotect_whole_area(const char* what, area_id area)
{
	area_info info;
	status_t status = get_area_info(area, &info);
	if (status != B_OK) {
		printf(MARKER ": %s: get_area_info failed: %s\n", what,
			strerror(status));
		return -1;
	}

	printf(MARKER ": %s: area %" B_PRId32 " at %p, %zu bytes, protection %#"
		B_PRIx32 "\n", what, area, info.address, info.size, info.protection);

	int returned = 0;

	for (size_t i = 0; i < kProtectionCount; i++) {
		printf(MARKER ": %s: mprotect(%p, %zu, %#x) ...\n", what, info.address,
			info.size, kProtections[i]);

		if (mprotect(info.address, info.size, kProtections[i]) != 0) {
			printf(MARKER ": %s: mprotect(%#x) failed: %s\n", what,
				kProtections[i], strerror(errno));
			continue;
		}

		printf(MARKER ": %s: mprotect(%#x) returned\n", what, kProtections[i]);
		returned++;
	}

	return returned > 0 ? 0 : -1;
}


/*!	Arm 1: ask a graphics driver to clone its framebuffer device area into our
	address space, then mprotect() it. Returns the number of areas exercised. */
static int
probe_graphics_frame_buffers(void)
{
	int exercised = 0;

	for (int i = 0; kGraphicsDevices[i] != NULL; i++) {
		const char* path = kGraphicsDevices[i];

		int fd = open(path, O_RDWR);
		if (fd < 0) {
			printf(MARKER ": gfx: cannot open %s: %s\n", path,
				strerror(errno));
			continue;
		}

		area_info info;
		memset(&info, 0, sizeof(info));
		if (ioctl(fd, VESA_CLONE_FRAME_BUFFER, &info, sizeof(info)) != 0) {
			printf(MARKER ": gfx: %s: VESA_CLONE_FRAME_BUFFER failed: %s\n",
				path, strerror(errno));
			close(fd);
			continue;
		}

		printf(MARKER ": gfx: %s cloned its frame buffer as area %" B_PRId32
			"\n", path, info.area);

		if (mprotect_whole_area("gfx", info.area) == 0)
			exercised++;

		delete_area(info.area);
		close(fd);
	}

	return exercised;
}


/*!	Arm 2: clone a cloneable device area into our own address space and
	mprotect() it. Returns the number of areas actually exercised. */
static int
probe_cloned_device_areas(void)
{
	int exercised = 0;

	for (int i = 0; kDeviceAreaNames[i] != NULL; i++) {
		const char* name = kDeviceAreaNames[i];

		area_id source = find_area(name);
		if (source < 0) {
			printf(MARKER ": clone: no area named \"%s\" on this machine (%s)\n",
				name, strerror(source));
			continue;
		}

		printf(MARKER ": clone: found \"%s\" as area %" B_PRId32 "\n", name,
			source);

		void* address = NULL;
		area_id clone = clone_area(MARKER " clone", &address, B_ANY_ADDRESS,
			B_READ_AREA, source);
		if (clone < 0) {
			printf(MARKER ": clone: clone_area(\"%s\") refused: %s\n", name,
				strerror(clone));
			continue;
		}

		if (mprotect_whole_area("clone", clone) == 0)
			exercised++;

		delete_area(clone);
	}

	return exercised;
}


/*!	Arm 3: map unbacked physical memory into our own address space through
	/dev/misc/poke and mprotect() it. Returns 1 if it got that far. */
static int
probe_poked_device_area(void)
{
	system_info sysInfo;
	status_t status = get_system_info(&sysInfo);
	if (status != B_OK) {
		printf(MARKER ": poke: get_system_info failed: %s\n", strerror(status));
		return 0;
	}

	const uint64 ramBytes = (uint64)sysInfo.max_pages * B_PAGE_SIZE;
	printf(MARKER ": poke: %" B_PRIu64 " MiB of RAM, mapping physical %#"
		B_PRIx64 "\n", ramBytes / (1024 * 1024), kUnbackedPhysicalAddress);

	if (kUnbackedPhysicalAddress <= ramBytes) {
		printf(MARKER ": poke: %#" B_PRIx64 " may be real RAM on this machine, "
			"refusing (the probe needs an address outside the page array)\n",
			kUnbackedPhysicalAddress);
		return 0;
	}

	int fd = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (fd < 0) {
		printf(MARKER ": poke: cannot open %s: %s%s\n", POKE_DEVICE_FULLNAME,
			strerror(errno),
			geteuid() != 0 ? " (expected: poke_open() is root-only)" : "");
		return 0;
	}

	mem_map_args args;
	memset(&args, 0, sizeof(args));
	args.signature = POKE_SIGNATURE;
	args.name = MARKER " poked";
	args.physical_address = kUnbackedPhysicalAddress;
	args.size = kUnbackedSize;
	args.flags = B_ANY_ADDRESS;
	args.protection = B_READ_AREA;

	if (ioctl(fd, POKE_MAP_MEMORY, &args, sizeof(args)) != 0) {
		printf(MARKER ": poke: POKE_MAP_MEMORY failed: %s\n", strerror(errno));
		close(fd);
		return 0;
	}

	int exercised = mprotect_whole_area("poke", args.area) == 0 ? 1 : 0;

	args.signature = POKE_SIGNATURE;
	ioctl(fd, POKE_UNMAP_MEMORY, &args, sizeof(args));
	close(fd);

	return exercised;
}


int
main(void)
{
	/* Unbuffered: a panic must not swallow the progress we already printed. */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	printf(MARKER ": start (euid %d)\n", (int)geteuid());

	/* Arm 1 first: it is the stronger claim, and opening
	   /dev/graphics/framebuffer is what makes the driver delete the boot
	   console's cloneable "frame buffer" area that arm 2 looks for. Arm 2 is
	   therefore expected to find nothing once arm 1 has run on a machine that
	   has the driver, which is the honest ordering rather than the flattering
	   one. */
	const int gfx = probe_graphics_frame_buffers();
	const int cloned = probe_cloned_device_areas();
	const int poked = probe_poked_device_area();

	printf(MARKER ": exercised %d driver-cloned, %d name-cloned and %d poked "
		"device area(s)\n", gfx, cloned, poked);

	if (gfx + cloned + poked == 0) {
		/* Nothing was built, so nothing was tested. This is not a pass: the
		   kernel was never asked the question. */
		printf(MARKER ": SKIP (no CACHE_TYPE_DEVICE area could be placed in "
			"this address space; the defect was not exercised)\n");
		return 2;
	}

	printf(MARKER ": PASS (mprotect() returned for every device area, kernel "
		"survived)\n");
	return 0;
}
