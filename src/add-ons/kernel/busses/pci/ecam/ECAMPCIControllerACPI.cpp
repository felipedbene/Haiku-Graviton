/*
 * Copyright 2022, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "ECAMPCIController.h"
#include <acpi.h>

#include <AutoDeleterDrivers.h>

#include "acpi_irq_routing_table.h"

#include <string.h>


status_t
ECAMPCIControllerACPI::ReadResourceInfo()
{
	DeviceNodePutter<&gDeviceManager> parent(gDeviceManager->get_parent_node(fNode));
	return ReadResourceInfo(parent.Get());
}


status_t
ECAMPCIControllerACPI::ReadResourceInfo(device_node* parent)
{
	dprintf("initialize PCI controller from ACPI\n");

	acpi_module_info* acpiModule;
	acpi_device_module_info* acpiDeviceModule;
	acpi_device acpiDevice;

	CHECK_RET(get_module(B_ACPI_MODULE_NAME, (module_info**)&acpiModule));

	acpi_mcfg *mcfg;
	CHECK_RET(acpiModule->get_table(ACPI_MCFG_SIGNATURE, 0, (void**)&mcfg));

	CHECK_RET(gDeviceManager->get_driver(parent, (driver_module_info**)&acpiDeviceModule,
		(void**)&acpiDevice));

	acpi_status acpi_res = acpiDeviceModule->walk_resources(acpiDevice, (char *)"_CRS",
		AcpiCrsScanCallback, this);

	if (acpi_res != 0)
		return B_ERROR;

	acpi_mcfg_allocation *end = (acpi_mcfg_allocation *) ((char*)mcfg + mcfg->header.length);
	acpi_mcfg_allocation *first = (acpi_mcfg_allocation *) (mcfg + 1);

	// An MCFG may hold one ECAM allocation per root bridge rather than one for
	// the machine. A 96-vCPU guest declares three root bridges -- _CRS bus
	// ranges 0-0, 1-43 and 44-56, each with its own disjoint MMIO windows -- and
	// three allocations covering exactly those ranges. ReadResourceInfo() runs
	// once per bridge, so a bridge that picks an allocation by anything other
	// than its own bus range picks the wrong one: all three took the first, all
	// three mapped bus 0, all three enumerated the same physical devices, and
	// one NVMe controller was published three times as disk/nvme/0, /1 and /2.
	// The machine booted from bus 0 and had no Ethernet controller anywhere.
	//
	// So pick the allocation covering the buses this bridge decodes, preferring
	// the tightest fit where more than one covers them.
	uint32 count = 0;
	acpi_mcfg_allocation *chosen = NULL;
	for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
		dprintf("PCI: ecam region: addr %" B_PRIx64 ", segment: %x, buses: %x-%x\n",
			alloc->address, alloc->pci_segment, alloc->start_bus_number,
			alloc->end_bus_number);

		count++;

		if (!fHaveCrsBusRange)
			continue;

		if (alloc->start_bus_number > fCrsBusStart
			|| alloc->end_bus_number < fCrsBusEnd) {
			continue;
		}

		const uint32 span = (uint32)alloc->end_bus_number
			- alloc->start_bus_number;
		if (chosen == NULL
			|| span < (uint32)(chosen->end_bus_number - chosen->start_bus_number))
			chosen = alloc;
	}

	if (chosen == NULL) {
		// Either firmware gave this bridge no bus range, or no allocation covers
		// it. Fall back to what this driver did before any of this: prefer
		// segment 0 where there is one, otherwise the first entry.
		for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
			if (chosen == NULL
				|| (chosen->pci_segment != 0 && alloc->pci_segment == 0))
				chosen = alloc;
		}

		if (chosen == NULL) {
			dprintf("PCI: MCFG describes no ECAM region!\n");
			return B_ERROR;
		}

		if (fHaveCrsBusRange) {
			dprintf("PCI: no ECAM region covers this bridge's buses %x-%x; "
				"falling back to segment %x buses %x-%x\n", fCrsBusStart,
				fCrsBusEnd, chosen->pci_segment, chosen->start_bus_number,
				chosen->end_bus_number);
		}
	}

	// A bridge's buses may still arrive in more than one piece sharing a base,
	// so take every piece that overlaps this bridge's range and record which
	// buses each piece actually claims. The pieces are not required to tile the
	// range they span, and a config access to a bus nothing decodes is not
	// guaranteed to read as all-ones -- it may abort -- so buses no piece
	// claimed must never be probed. On the machine that prompted this the pieces
	// tile 0-56 exactly, which was luck rather than design; the bitmap makes it
	// safe by construction.
	uint32 startBus = chosen->start_bus_number;
	uint32 endBus = chosen->end_bus_number;
	uint32 pieces = 0;
	uint32 dropped = 0;

	ClearValidBuses();
	for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
		const bool sameWindow = alloc->address == chosen->address
			&& alloc->pci_segment == chosen->pci_segment;
		const bool overlapsUs = alloc->start_bus_number <= endBus
			&& alloc->end_bus_number >= startBus;

		if (sameWindow && overlapsUs) {
			if (alloc->start_bus_number < startBus)
				startBus = alloc->start_bus_number;
			if (alloc->end_bus_number > endBus)
				endBus = alloc->end_bus_number;
			for (uint32 bus = alloc->start_bus_number;
					bus <= alloc->end_bus_number; bus++) {
				SetBusValid(bus);
			}
			pieces++;
		} else if (!sameWindow) {
			// A separate base or segment is a genuinely separate ECAM window,
			// and this bridge maps one. Name it and what is lost: devices behind
			// these buses will simply never be found, which is invisible unless
			// it is said here.
			dprintf("PCI: ECAM region addr %" B_PRIx64 " segment %x buses %x-%x "
				"is a separate window; not mapped by this bridge\n",
				alloc->address, alloc->pci_segment, alloc->start_bus_number,
				alloc->end_bus_number);
			dropped++;
		}
	}

	if (pieces > 1) {
		dprintf("PCI: joined %" B_PRIu32 " ECAM pieces at %" B_PRIx64
			" into buses %x-%x\n", pieces, chosen->address, startBus, endBus);
	}

	fStartBusNumber = (uint8)startBus;
	fEndBusNumber = (uint8)endBus;

	// The base is the address of bus 0, not of the entry's start bus. That is
	// forced by the data: several entries report the *same* base with different
	// start buses, which cannot each be "the address of my start bus" without
	// putting several apertures at one physical address, but is consistent as
	// one aperture based at bus 0 whose buses firmware listed in pieces. So the
	// mapping begins at the start bus's offset into that aperture, and
	// ConfigAddress() rebases absolute bus numbers onto it. For a single entry
	// starting at bus 0 -- every guest, and metal -- both readings agree and
	// this is a no-op.
	fBusOffset = (uint8)startBus;

	const phys_addr_t base = chosen->address + ((uint64)startBus << 20);
	fRegsLen = (uint64(fEndBusNumber) - fStartBusNumber + 1) << 20;
	fRegsArea.SetTo(map_physical_memory("PCI Config MMIO",
		base, fRegsLen, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void **)&fRegs));
	CHECK_RET(fRegsArea.Get());

	uint32 decoded = 0;
	for (uint32 bus = startBus; bus <= endBus; bus++) {
		if (IsBusValid(bus))
			decoded++;
	}

	dprintf("PCI: ECAM at %" B_PRIx64 " (bus %x base %" B_PRIx64 "), segment %x,"
		" buses %x-%x, %" B_PRIu32 " decoded, %" B_PRIu64 " MiB\n",
		chosen->address, startBus, base, chosen->pci_segment, fStartBusNumber,
		fEndBusNumber, decoded, fRegsLen >> 20);

	if (dropped > 0) {
		dprintf("PCI: %" B_PRIu32 " of %" B_PRIu32 " ECAM regions belong to "
			"other windows\n", dropped, count);
	}

	return B_OK;
}


acpi_status
ECAMPCIControllerACPI::AcpiCrsScanCallback(acpi_resource *res, void *context)
{
	return static_cast<ECAMPCIControllerACPI*>(context)->AcpiCrsScanCallbackInt(res);
}


/** Convert an ACPI address resource descriptor into a pci_resource_range.
 *
 * This is a template because ACPI resources can be encoded using 8, 16, 32 or 64 bit values.
 */
template<typename T> bool
DecodeAddress(const T& resource, pci_resource_range& range)
{
	const auto& acpiRange = resource.address;
	dprintf("PCI: range from ACPI [%lx(%d),%lx(%d)] with length %lx\n",
		(unsigned long)acpiRange.minimum, resource.minAddress_fixed,
		(unsigned long)acpiRange.maximum, resource.maxAddress_fixed,
		(unsigned long)acpiRange.address_length);

	// If address_length isn't set, compute it from minimum and maximum
	// If maximum isn't set, compute it from minimum and length
	auto addressLength = acpiRange.address_length;
	phys_addr_t addressMaximum = acpiRange.maximum;

	if (addressLength == 0 && addressMaximum <= acpiRange.minimum) {
		// There's nothing we can do with that...
		dprintf("PCI: Ignore empty ACPI range\n");
		return false;
	} else if (!resource.maxAddress_fixed) {
		if (addressLength == 0) {
			dprintf("PCI: maxAddress and addressLength are not set, ignore range\n");
			return false;
		}

		dprintf("PCI: maxAddress is not set, compute it\n");
		addressMaximum = acpiRange.minimum + addressLength - 1;
	} else if (addressLength != addressMaximum - acpiRange.minimum + 1) {
		dprintf("PCI: Fixup invalid length from ACPI!\n");
		addressLength = addressMaximum - acpiRange.minimum + 1;
	}

	range.host_address = acpiRange.minimum + acpiRange.translation_offset;
	range.pci_address  = acpiRange.minimum;
	range.size = addressLength;

	return true;
}


acpi_status
ECAMPCIControllerACPI::AcpiCrsScanCallbackInt(acpi_resource *res)
{
	pci_resource_range range = {};

	switch (res->type) {
		case ACPI_RESOURCE_TYPE_ADDRESS16: {
			const auto& address = res->data.address16;
			if (!DecodeAddress(address, range))
				return B_OK;
			break;
		}
		case ACPI_RESOURCE_TYPE_ADDRESS32: {
			const auto& address = res->data.address32;
			if (!DecodeAddress(address, range))
				return B_OK;
			range.address_type |= PCI_address_type_32;
			break;
		}
		case ACPI_RESOURCE_TYPE_ADDRESS64: {
			const auto& address = res->data.address64;
			if (!DecodeAddress(address, range))
				return B_OK;
			range.address_type |= PCI_address_type_64;
			break;
		}

		default:
			return B_OK;
	}

	switch (res->data.address.resource_type) {
		case 0: // ACPI_MEMORY_RANGE
			range.type = B_IO_MEMORY;
			if (res->data.address.info.mem.caching == 3 /*ACPI_PREFETCHABLE_MEMORY*/)
				range.address_type |= PCI_address_prefetchable;
			break;
		case 1: // ACPI_IO_RANGE
			range.type = B_IO_PORT;
			break;

		case 2: // ACPI_BUS_NUMBER_RANGE
			// Not a resource to hand out to devices, so it does not belong in
			// fResourceRanges -- but it is the only thing that says which buses
			// this root bridge is responsible for, and an MCFG may hold one
			// allocation per bridge. Keep it; ReadResourceInfo() picks the
			// matching allocation with it.
			if (range.size > 0) {
				fCrsBusStart = (uint32)range.pci_address;
				fCrsBusEnd = (uint32)(range.pci_address + range.size - 1);
				fHaveCrsBusRange = true;
			}
			return B_OK;

		default:
			return B_OK;
	}

	fResourceRanges.Add(range);
	return B_OK;
}


status_t
ECAMPCIControllerACPI::Finalize()
{
	dprintf("finalize PCI controller from ACPI\n");

	acpi_module_info *acpiModule;
	CHECK_RET(get_module(B_ACPI_MODULE_NAME, (module_info**)&acpiModule));

	IRQRoutingTable table;
	CHECK_RET(prepare_irq_routing(acpiModule, table, [](int32 gsi) {return true;}));

	CHECK_RET(enable_irq_routing(acpiModule, table));

	print_irq_routing_table(table);

	return B_OK;
}
