/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The out-of-line half of the Haiku platform layer for ena-com. See
 * ena-com/ena_plat.h for the contract and the reasoning behind it.
 */


#include <KernelExport.h>
#include <kernel.h>

#include <util/Random.h>
#include <vm/vm.h>

#include "ena.h"


int ena_log_level = ENA_INFO;


/*!	Allocates coherent memory for a descriptor ring or an admin buffer, and
	reports its physical address.

	ena-com hands us an alignment but no address bound, so we apply the bound
	ourselves: the device advertises how many physical address bits it can
	drive, and DMA to an address wider than that would be silently truncated
	into unrelated memory. dmadev is the driver device, which is where the
	negotiated width lives.
*/
int
ena_dma_alloc(void* dmadev, size_t size, ena_mem_handle_t* dma, int mapflags,
	size_t alignment, int domain)
{
	ena_haiku_device* device = (ena_haiku_device*)dmadev;

	dma->vaddr = NULL;
	dma->paddr = 0;
	dma->area = -1;
	dma->size = 0;

	size = ROUNDUP(size, B_PAGE_SIZE);

	virtual_address_restrictions virtualRestrictions = {};

	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.alignment = alignment;
	if (device != NULL && device->dmaWidth > 0
			&& device->dmaWidth < (sizeof(phys_addr_t) * 8)) {
		physicalRestrictions.high_address
			= (phys_addr_t)1 << device->dmaWidth;
	}

	void* address = NULL;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "ena dma buffer", size,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < B_OK) {
		dprintf("ena: failed to allocate %" B_PRIuSIZE " bytes of DMA memory: "
			"%s\n", size, strerror(area));
		return ENA_COM_NO_MEM;
	}

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		dprintf("ena: cannot resolve physical address of DMA buffer: %s\n",
			strerror(status));
		delete_area(area);
		return ENA_COM_NO_MEM;
	}

	memset(address, 0, size);

	dma->vaddr = address;
	dma->paddr = entry.address;
	dma->area = area;
	dma->size = size;

	return ENA_COM_OK;
}


void
ena_dma_free(void* dmadev, ena_mem_handle_t* dma)
{
	if (dma->area >= 0)
		delete_area(dma->area);

	dma->vaddr = NULL;
	dma->paddr = 0;
	dma->area = -1;
	dma->size = 0;
}


/*!	Fills the RSS hash key.

	Only reached if RSS is configured. The key is not a secret in any
	meaningful sense -- it only decides which queue a flow lands on -- but
	drawing it from the kernel's pool costs nothing and avoids every instance
	hashing identically.
*/
void
ena_rss_key_fill(void* key, size_t size)
{
	uint8* bytes = (uint8*)key;

	for (size_t i = 0; i < size; i++)
		bytes[i] = (uint8)(secure_random_value() & 0xff);
}
