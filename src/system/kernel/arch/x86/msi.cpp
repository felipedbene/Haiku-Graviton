/*
 * Copyright 2010-2011, Michael Lotz, mmlr@mlotz.ch. All Rights Reserved.
 * Distributed under the terms of the MIT license.
 */

#include <arch/x86/apic.h>
#include <arch/x86/msi.h>
#include <arch/x86/arch_smp.h>

#include <debug.h>
#include <interrupts.h>
#include <lock.h>


struct MSIConfiguration {
	uint64*	fAddress;
	uint32* fData;
};

static MSIConfiguration sMSIConfigurations[NUM_IO_VECTORS];

static bool sMSISupported = false;
static uint32 sBootCPUAPICId = 0;


void
msi_init(kernel_args* args)
{
	if (!apic_available()) {
		dprintf("disabling msi due to missing apic\n");
		return;
	}

	dprintf("msi support enabled\n");
	sMSISupported = true;
	sBootCPUAPICId = args->arch_args.cpu_apic_id[0];
}


bool
msi_supported()
{
	return sMSISupported;
}


status_t
msi_allocate_vectors(uint32 count, uint32 *startVector, uint64 *address,
	uint32 *data)
{
	if (!sMSISupported)
		return B_UNSUPPORTED;

	int32 vector;
	status_t result = allocate_io_interrupt_vectors(count, &vector,
		INTERRUPT_TYPE_IRQ);
	if (result != B_OK)
		return result;

	if (vector >= NUM_IO_VECTORS) {
		free_io_interrupt_vectors(count, vector);
		return B_NO_MEMORY;
	}

	sMSIConfigurations[vector].fAddress = address;
	sMSIConfigurations[vector].fData = data;
	x86_set_irq_source(vector, IRQ_SOURCE_MSI);

	*startVector = (uint32)vector;
	*address = MSI_ADDRESS_BASE | (sBootCPUAPICId << MSI_DESTINATION_ID_SHIFT)
		| MSI_NO_REDIRECTION | MSI_DESTINATION_MODE_PHYSICAL;
	*data = MSI_TRIGGER_MODE_EDGE | MSI_DELIVERY_MODE_FIXED
		| ((uint16)vector + ARCH_INTERRUPT_BASE);

	dprintf("msi_allocate_vectors: allocated %" B_PRIu32 " vectors starting from %" B_PRIu32 "\n",
		count, *startVector);
	return B_OK;
}


/*!	Allocates MSI vectors on behalf of a specific PCI requester.

	x86 has no interrupt-translation hardware in the message path: an MSI write
	targets a local APIC directly, encoded in the address and data the caller
	programs into the device. Nothing needs to know which device sent it, so the
	requester id is deliberately ignored here.

	This exists so that the shared PCI bus manager can call one function on every
	architecture. On the arm64/riscv64 side the same entry point is implemented by
	arch/generic/generic_msi.cpp, where the requester id *is* load-bearing --
	a GICv3 ITS keys its device table on it and cannot route an MSI without it.
	generic_msi.cpp is not built for x86, so without this the pci add-on has an
	undefined reference and no PCI device works at all.
*/
status_t
msi_allocate_vectors_for_device(uint32 requesterID, uint32 count,
	uint32 *startVector, uint64 *address, uint32 *data)
{
	return msi_allocate_vectors(count, startVector, address, data);
}


void
msi_free_vectors(uint32 count, uint32 startVector)
{
	if (!sMSISupported) {
		panic("trying to free msi vectors but msi not supported\n");
		return;
	}

	dprintf("msi_free_vectors: freeing %" B_PRIu32 " vectors starting from %" B_PRIu32 "\n", count,
		startVector);

	free_io_interrupt_vectors(count, startVector);
}


void
msi_assign_interrupt_to_cpu(uint32 irq, int32 cpu)
{
	uint32 apic_id = x86_get_cpu_apic_id(cpu);

	uint64* address = sMSIConfigurations[irq].fAddress;
	*address = MSI_ADDRESS_BASE | (apic_id << MSI_DESTINATION_ID_SHIFT)
		| MSI_NO_REDIRECTION | MSI_DESTINATION_MODE_PHYSICAL;
}

