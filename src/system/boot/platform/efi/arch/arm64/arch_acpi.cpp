/*
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Released under the terms of the MIT License.
 */

#include "string.h"

#include <boot/platform.h>
#include <boot/stage2.h>
#include <arch_acpi.h>
#include <arch_smp.h>

#include "serial.h"
#include "acpi.h"

#include <arch/arm/arch_uart_pl011.h>
#include <arch/generic/debug_uart_8250.h>

#include <stddef.h>

// ARM_BOOT_ARCH sits at a fixed offset in the FADT; guard the struct against
// accidental drift.
static_assert(offsetof(acpi_fadt, arm_boot_arch) == 129,
	"acpi_fadt layout does not match the ACPI specification");


static void arch_acpi_get_uart_pl011(const uart_info &uart)
{
	static char sUART[sizeof(ArchUARTPL011)];
	gUART = new(sUART) ArchUARTPL011(uart.regs.start,
		uart.clock != 0 ? uart.clock : 0x16e3600);
}


static void arch_acpi_get_uart_8250(const uart_info &uart)
{
	// SPCR may report a clock of zero, in which case assume the classic
	// 16550 input clock of 1.8432 MHz.
	static char sUART[sizeof(DebugUART8250)];
	gUART = new(sUART) DebugUART8250(uart.regs.start,
		uart.clock != 0 ? uart.clock : 1843200, uart.reg_shift);
}


// SPCR interface types and DBG2 serial port subtypes are numbered alike.
static const char *
arch_acpi_uart_kind(uint32 interfaceType)
{
	switch (interfaceType) {
		case ACPI_SPCR_INTERFACE_TYPE_16550:
		case ACPI_SPCR_INTERFACE_TYPE_16550_SUBSET:
			return UART_KIND_8250;
		case ACPI_SPCR_INTERFACE_TYPE_PL011:
		case ACPI_SPCR_INTERFACE_TYPE_SBSA_32BIT:
		case ACPI_SPCR_INTERFACE_TYPE_SBSA:
			return UART_KIND_PL011;
	}

	return NULL;
}


// The generic address structure describes how wide a single register is, which
// for a 16550 is also how far apart consecutive registers sit: byte-packed on
// AWS Graviton, 32-bit spaced on the SoCs the driver was originally written
// for. Report "unknown" for anything else and let the driver keep its default.
static int8
arch_acpi_uart_reg_shift(const acpi_gas &address)
{
	if (address.access_size == ACPI_GAS_ACCESS_SIZE_BYTE
		|| address.bit_width == 8) {
		return 0;
	}

	if (address.access_size == ACPI_GAS_ACCESS_SIZE_WORD
		|| address.bit_width == 16) {
		return 1;
	}

	if (address.access_size == ACPI_GAS_ACCESS_SIZE_DWORD
		|| address.bit_width == 32) {
		return 2;
	}

	return UART_REG_SHIFT_UNSET;
}


static void
arch_acpi_setup_uart(uart_info &uart, const char *kind)
{
	// Firmware handed us a console it has already programmed (SPCR/DBG2 only
	// describe consoles that are in use), so re-running InitPort would only
	// risk clobbering a working setup -- and on a byte-strided 16550 its DLAB
	// dance transmits a stray byte.
	gUARTSkipInit = true;

	if (strcmp(kind, UART_KIND_PL011) == 0)
		arch_acpi_get_uart_pl011(uart);
	else if (strcmp(kind, UART_KIND_8250) == 0)
		arch_acpi_get_uart_8250(uart);
}


// Turn the per-CPU redistributor base addresses from the MADT's GICC entries
// into the smallest set of contiguous regions that covers them. Firmware only
// uses that form when a single GICR structure would not do -- on AWS Graviton3
// the 64 redistributors sit in two runs of 32 about 16 GiB apart -- so the
// gaps are real and must survive into the kernel.
//
// The distance between neighbours is taken from the addresses themselves
// rather than from the GIC version, because it is the one thing the firmware
// states unambiguously: a GICv3 PE owns two 64 KB frames and a GICv4 PE four,
// and a run whose neighbours are that far apart is contiguous by definition.
static void
arch_acpi_set_gicr_regions(intc_info &intc, const uint64 *bases, uint32 count,
	uint8 version)
{
	intc.gicr_region_count = 0;
	if (count == 0)
		return;

	// Insertion sort; the MADT is not required to list the CPUs in address
	// order and coalescing needs them to be.
	static uint64 sorted[SMP_MAX_CPUS];
	for (uint32 i = 0; i < count; i++) {
		uint32 j = i;
		while (j > 0 && sorted[j - 1] > bases[i]) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = bases[i];
	}

	const uint64 kStrideV3 = 0x20000;
	const uint64 kStrideV4 = 0x40000;

	uint32 regions = 0;
	for (uint32 i = 0; i < count; ) {
		uint64 stride = 0;
		uint32 j = i + 1;
		while (j < count) {
			const uint64 delta = sorted[j] - sorted[j - 1];
			if (delta != kStrideV3 && delta != kStrideV4)
				break;
			if (stride == 0)
				stride = delta;
			else if (delta != stride)
				break;
			j++;
		}

		// A region holding a single redistributor says nothing about the
		// spacing, so fall back on what the distributor claims to be.
		if (stride == 0)
			stride = (version >= 4) ? kStrideV4 : kStrideV3;

		if (regions >= (uint32)INTC_MAX_GICR_REGIONS) {
			dprintf("acpi: more than %d gic redistributor regions; the CPUs "
				"behind the rest will not be usable\n",
				INTC_MAX_GICR_REGIONS);
			break;
		}

		intc.gicr_regions[regions].start = sorted[i];
		intc.gicr_regions[regions].size = (sorted[j - 1] - sorted[i]) + stride;
		dprintf("  gicr region %u: %lx (size %lx), %u redistributors, "
			"stride %lx\n", regions, intc.gicr_regions[regions].start,
			intc.gicr_regions[regions].size, j - i, stride);
		regions++;

		i = j;
	}

	intc.gicr_region_count = regions;
}


void
arch_handle_acpi()
{
	acpi_spcr *spcr = (acpi_spcr*)acpi_find_table(ACPI_SPCR_SIGNATURE);
	if (spcr != NULL) {
		uart_info &uart = gKernelArgs.arch_args.uart;
		const char *kind = arch_acpi_uart_kind(spcr->interface_type);

		if (kind != NULL)
			strcpy(uart.kind, kind);

		uart.regs.start = spcr->base_address.address;
		uart.regs.size = B_PAGE_SIZE;
		uart.irq = spcr->gisv;
		uart.clock = spcr->clock;
		uart.reg_shift = arch_acpi_uart_reg_shift(spcr->base_address);

		if (kind != NULL)
			arch_acpi_setup_uart(uart, kind);

		dprintf("discovered uart from acpi: base=%lx, irq=%u, clock=%lu, "
			"reg_shift=%d\n", uart.regs.start, uart.irq, uart.clock,
			uart.reg_shift);
	} else {
		acpi_dbg2 *dbg2 = (acpi_dbg2*)acpi_find_table(ACPI_DBG2_SIGNATURE);
		if (dbg2 != NULL) {
			acpi_dbg2_device_info *info = (acpi_dbg2_device_info*)((char*)dbg2
				+ dbg2->offset_dbg_device_info);
			while (info != (acpi_dbg2_device_info*)((char*)dbg2 + dbg2->header.length)) {
				if (info->port_type == ACPI_DBG2_PORT_TYPE_SERIAL && info->num_addresses > 0) {
					uart_info &uart = gKernelArgs.arch_args.uart;
					const char *kind = arch_acpi_uart_kind(info->port_subtype);

					if (kind != NULL)
						strcpy(uart.kind, kind);

					acpi_gas *base_addr = (acpi_gas*)((char*)info + info->base_addr_offset);
					uint32 *base_size = (uint32*)((char*)info + info->addr_size_offset);

					uart.regs.start = base_addr->address;
					uart.regs.size = *base_size;
					uart.irq = 0;
					uart.clock = 0;
					uart.reg_shift = arch_acpi_uart_reg_shift(*base_addr);

					if (kind != NULL)
						arch_acpi_setup_uart(uart, kind);

					dprintf("discovered uart from dbg2 acpi: base=%lx, "
						"reg_shift=%d\n", uart.regs.start, uart.reg_shift);
					break;
				}

				info = (acpi_dbg2_device_info*)((char*)info + info->length);
			}
		}
	}

	acpi_madt *madt = (acpi_madt*)acpi_find_table(ACPI_MADT_SIGNATURE);
	if (madt != NULL) {
		uint64 gicc_base = 0;
		uint64 gicd_base = 0;
		uint64 gicr_base = 0;
		uint64 gicr_size = 0;
		uint64 its_base = 0;
		uint8 version = 0;

		// Redistributor base addresses collected from the GICC entries, for
		// firmware that describes them per-CPU rather than with a GICR
		// structure. Static rather than automatic: the boot loader's stack is
		// not generous enough to spend half a kilobyte on it.
		static uint64 gicr_bases[SMP_MAX_CPUS];
		uint32 gicr_base_count = 0;
		bool reportedTooManyCpus = false;

		acpi_apic *desc = (acpi_apic*)(madt + 1);
		while (desc != (acpi_apic*)((char*)madt + madt->header.length)) {
			if (desc->type == ACPI_MADT_GIC_INTERFACE) {
				acpi_gic_interface *acpi_gicc = (acpi_gic_interface*)desc;
				if (acpi_gicc->cpu_interface_num == 0)
					gicc_base = acpi_gicc->base_address;

				// A CPU we have no room for must not abort the rest of this
				// walk: the entry still has to be stepped over, or the loop
				// never advances and the loader spins here for ever.
				platform_cpu_info* cpu = NULL;
				arch_smp_register_cpu(&cpu);
				if (cpu == NULL) {
					if (!reportedTooManyCpus) {
						dprintf("acpi: the MADT describes more CPUs than this "
							"build supports (%d); ignoring the rest\n",
							SMP_MAX_CPUS);
						reportedTooManyCpus = true;
					}
				} else {
					cpu->id = acpi_gicc->cpu_interface_num;
					cpu->mpidr = acpi_gicc->mpidr;

					// Firmware may describe the redistributors per-CPU here
					// instead of via a GICR structure. Remember every base:
					// unlike a GICR structure, these are not required to
					// describe one contiguous range.
					if (acpi_gicc->gicr_address != 0
						&& gicr_base_count < SMP_MAX_CPUS) {
						gicr_bases[gicr_base_count++] = acpi_gicc->gicr_address;
						if (gicr_base == 0 || acpi_gicc->gicr_address < gicr_base)
							gicr_base = acpi_gicc->gicr_address;
					}
				}
			} else if (desc->type == ACPI_MADT_GIC_DISTRIBUTOR) {
				acpi_gic_distributor *acpi_gicd = (acpi_gic_distributor*)desc;
				gicd_base = acpi_gicd->base_address;
				version = acpi_gicd->gic_version;
			} else if (desc->type == ACPI_MADT_GIC_ITS) {
				// Only the first ITS is used; a single one can serve every
				// device we care about.
				acpi_gic_its *acpi_its = (acpi_gic_its*)desc;
				if (its_base == 0)
					its_base = acpi_its->base_address;
			} else if (desc->type == ACPI_MADT_GIC_REDISTRIBUTOR) {
				acpi_gic_redistributor *acpi_gicr
					= (acpi_gic_redistributor*)desc;
				gicr_base = acpi_gicr->base_address;
				gicr_size = acpi_gicr->range_length;
			}
			desc = (acpi_apic*)((char*)desc + desc->length);
		}

		intc_info &intc = gKernelArgs.arch_args.interrupt_controller;
		if (version == 2 && gicc_base != 0 && gicd_base != 0) {
			strcpy(intc.kind, INTC_KIND_GICV2);
			intc.regs1.start = gicd_base;
			intc.regs2.start = gicc_base;

			dprintf("discovered gic from acpi: version=%d, gicd=%lx, gicc=%lx\n",
				version, gicd_base, gicc_base);
		} else if (gicd_base != 0 && gicr_base != 0
				&& (version >= 3 || version == 0)) {
			// A GIC version of 0 means "unspecified"; the presence of
			// redistributors is what actually distinguishes v3 from v2.
			strcpy(intc.kind, INTC_KIND_GICV3);
			intc.regs1.start = gicd_base;
			intc.regs1.size = 0x10000;
			intc.regs2.start = gicr_base;
			intc.regs2.size = gicr_size;
			intc.regs3.start = its_base;
			intc.regs3.size = its_base != 0 ? 0x20000 : 0;

			dprintf("discovered gic from acpi: version=%d, gicd=%lx, "
				"gicr=%lx (size %lx), its=%lx\n", version, gicd_base,
				gicr_base, gicr_size, its_base);

			arch_acpi_set_gicr_regions(intc, gicr_bases, gicr_base_count,
				version);
		}
	}

	// Without a device tree there is no "enable-method" property to consult,
	// so the only indication that secondary CPUs can be started via PSCI --
	// and whether to use SMC or HVC -- is the FADT's ARM_BOOT_ARCH field.
	acpi_fadt *fadt = (acpi_fadt*)acpi_find_table(ACPI_FADT_SIGNATURE);
	if (fadt != NULL && fadt->header.length > offsetof(acpi_fadt, minor_version)
		&& (fadt->arm_boot_arch & ACPI_FADT_ARM_PSCI_COMPLIANT) != 0) {
		bool useHvc = (fadt->arm_boot_arch & ACPI_FADT_ARM_PSCI_USE_HVC) != 0;
		arch_smp_set_psci_conduit(useHvc);

		dprintf("discovered psci from acpi: conduit=%s\n",
			useHvc ? "hvc" : "smc");
	}
}
