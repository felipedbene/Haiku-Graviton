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

	// _SEG names the segment group this bridge lives in; absent means 0.
	uint32 segment = 0;
	bool haveSegment = false;
	{
		acpi_object_type object;
		acpi_data buffer = {sizeof(object), &object};
		if (acpiDeviceModule->evaluate_method(acpiDevice, "_SEG", NULL, &buffer) == B_OK
			&& object.object_type == ACPI_TYPE_INTEGER) {
			segment = (uint32)object.integer.integer;
			haveSegment = true;
		}
	}

	acpi_mcfg_allocation *end = (acpi_mcfg_allocation *) ((char*)mcfg + mcfg->header.length);
	acpi_mcfg_allocation *first = (acpi_mcfg_allocation *) (mcfg + 1);

	uint32 count = 0;
	bool severalSegments = false;
	for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
		dprintf("PCI: ecam region: addr %" B_PRIx64 ", segment: %x, buses: %x-%x\n",
			alloc->address, alloc->pci_segment, alloc->start_bus_number,
			alloc->end_bus_number);
		if (alloc->pci_segment != first->pci_segment)
			severalSegments = true;
		count++;
	}

	if (count == 0) {
		dprintf("PCI: MCFG describes no ECAM region!\n");
		return B_ERROR;
	}

	// The segment only selects anything when the MCFG names more than one. A
	// machine whose entries all carry the same number is completely described
	// by them however firmware numbered it: Graviton3 metal calls its only
	// region segment 1 and its bridge has no _SEG, and rejecting that left the
	// machine with no PCI at all.
	const bool matchSegment = severalSegments;

	// The bridge's _CRS says which buses it decodes; the MCFG only says where
	// config space for a segment's buses lives. So _CRS decides the root bus and
	// the mapped range, and the MCFG supplies the base -- the same division of
	// labour as Linux (pci_mcfg_lookup / pci_acpi_setup_ecam_mapping). Taking
	// the range from the MCFG entry instead only works where firmware happens to
	// emit one entry per bridge; with one entry per segment and several bridges,
	// every bridge would map buses 0-255 and enumerate the same devices.
	acpi_mcfg_allocation *chosen = NULL;
	bool fromCrs = false;

	if (fHaveCrsBusRange) {
		// Tightest entry covering the whole range; failing that, the first one
		// overlapping it (its buses may be listed in pieces).
		acpi_mcfg_allocation *overlapping = NULL;
		for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
			if (matchSegment && alloc->pci_segment != segment)
				continue;
			if (alloc->start_bus_number > fCrsBusEnd
				|| alloc->end_bus_number < fCrsBusStart) {
				continue;
			}
			if (overlapping == NULL)
				overlapping = alloc;
			if (alloc->start_bus_number > fCrsBusStart
				|| alloc->end_bus_number < fCrsBusEnd) {
				continue;
			}
			if (chosen == NULL
				|| (alloc->end_bus_number - alloc->start_bus_number)
					< (chosen->end_bus_number - chosen->start_bus_number)) {
				chosen = alloc;
			}
		}
		if (chosen == NULL)
			chosen = overlapping;
		fromCrs = chosen != NULL;
	}

	if (chosen == NULL) {
		// Say why, both ways: a fallback that is silent cannot be ruled out from
		// a console log.
		if (fHaveCrsBusRange) {
			dprintf("PCI: no ECAM region covers segment %" B_PRIx32 " buses %"
				B_PRIx32 "-%" B_PRIx32 "; falling back to MCFG entry\n", segment,
				fCrsBusStart, fCrsBusEnd);
		} else {
			dprintf("PCI: _CRS gives no bus range; falling back to MCFG entry\n");
		}

		// What this driver did before: the bridge's segment if known, else
		// segment 0, else the first entry.
		const uint32 wanted = haveSegment ? segment : 0;
		for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
			if (chosen == NULL
				|| (chosen->pci_segment != wanted && alloc->pci_segment == wanted))
				chosen = alloc;
		}
	}

	uint32 startBus;
	uint32 endBus;
	if (fromCrs) {
		startBus = fCrsBusStart;
		endBus = fCrsBusEnd > 0xff ? 0xff : fCrsBusEnd;
	} else {
		startBus = chosen->start_bus_number;
		endBus = chosen->end_bus_number;
	}

	// Only buses that are both this bridge's and claimed by some piece of the
	// same window get probed: a config access to a bus nothing decodes is not
	// guaranteed to read as all-ones and may abort. The range is fixed before
	// this loop, so the result does not depend on the order of the entries.
	uint32 decoded = 0;
	uint32 otherWindows = 0;
	ClearValidBuses();
	for (acpi_mcfg_allocation *alloc = first; alloc + 1 <= end; alloc++) {
		if (alloc->address != chosen->address
			|| alloc->pci_segment != chosen->pci_segment) {
			otherWindows++;
			continue;
		}
		for (uint32 bus = alloc->start_bus_number;
				bus <= alloc->end_bus_number; bus++) {
			if (bus >= startBus && bus <= endBus && !IsBusValid(bus)) {
				SetBusValid(bus);
				decoded++;
			}
		}
	}

	if (decoded == 0) {
		dprintf("PCI: no ECAM-decoded bus in %" B_PRIx32 "-%" B_PRIx32 "\n",
			startBus, endBus);
		return B_ERROR;
	}

	fStartBusNumber = (uint8)startBus;
	fEndBusNumber = (uint8)endBus;

	// The MCFG base is the address of bus 0 of the segment (PCI Firmware spec),
	// not of the entry's start bus, so the mapping begins at the start bus's
	// offset and ConfigAddress() rebases absolute bus numbers onto it.
	fBusOffset = (uint8)startBus;

	const phys_addr_t base = chosen->address + ((uint64)startBus << 20);
	fRegsLen = (uint64)(endBus - startBus + 1) << 20;
	fRegsArea.SetTo(map_physical_memory("PCI Config MMIO",
		base, fRegsLen, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void **)&fRegs));
	CHECK_RET(fRegsArea.Get());

	dprintf("PCI: ECAM at %" B_PRIx64 " (bus %" B_PRIx32 " base %" B_PRIx64
		"), segment %x, buses %" B_PRIx32 "-%" B_PRIx32 " from %s, %" B_PRIu32
		" decoded, %" B_PRIu64 " MiB; %" B_PRIu32 " of %" B_PRIu32
		" MCFG entries are other windows\n",
		chosen->address, startBus, (uint64)base, chosen->pci_segment, startBus,
		endBus, fromCrs ? "_CRS" : "MCFG", decoded, fRegsLen >> 20,
		otherWindows, count);

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
