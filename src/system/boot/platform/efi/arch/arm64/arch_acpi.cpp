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
		uart.clock != 0 ? uart.clock : 1843200);
}


void
arch_handle_acpi()
{
	acpi_spcr *spcr = (acpi_spcr*)acpi_find_table(ACPI_SPCR_SIGNATURE);
	if (spcr != NULL) {
		uart_info &uart = gKernelArgs.arch_args.uart;

		if (spcr->interface_type == ACPI_SPCR_INTERFACE_TYPE_PL011) {
			strcpy(uart.kind, UART_KIND_PL011);
		} else if (spcr->interface_type == ACPI_SPCR_INTERFACE_TYPE_16550) {
			strcpy(uart.kind, UART_KIND_8250);
		}

		uart.regs.start = spcr->base_address.address;
		uart.regs.size = B_PAGE_SIZE;
		uart.irq = spcr->gisv;
		uart.clock = spcr->clock;

		if (spcr->interface_type == ACPI_SPCR_INTERFACE_TYPE_PL011)
			arch_acpi_get_uart_pl011(uart);
		else if (spcr->interface_type == ACPI_SPCR_INTERFACE_TYPE_16550)
			arch_acpi_get_uart_8250(uart);

		dprintf("discovered uart from acpi: base=%lx, irq=%u, clock=%lu\n",
			uart.regs.start, uart.irq, uart.clock);
	} else {
		acpi_dbg2 *dbg2 = (acpi_dbg2*)acpi_find_table(ACPI_DBG2_SIGNATURE);
		if (dbg2 != NULL) {
			acpi_dbg2_device_info *info = (acpi_dbg2_device_info*)((char*)dbg2
				+ dbg2->offset_dbg_device_info);
			while (info != (acpi_dbg2_device_info*)((char*)dbg2 + dbg2->header.length)) {
				if (info->port_type == ACPI_DBG2_PORT_TYPE_SERIAL && info->num_addresses > 0) {
					uart_info &uart = gKernelArgs.arch_args.uart;

					if (info->port_subtype == ACPI_DBG2_PORT_SUBTYPE_PL011)
						strcpy(uart.kind, UART_KIND_PL011);
					else if (info->port_subtype == ACPI_DBG2_PORT_SUBTYPE_16550)
						strcpy(uart.kind, UART_KIND_8250);

					acpi_gas *base_addr = (acpi_gas*)((char*)info + info->base_addr_offset);
					uint32 *base_size = (uint32*)((char*)info + info->addr_size_offset);

					uart.regs.start = base_addr->address;
					uart.regs.size = *base_size;
					uart.irq = 0;
					uart.clock = 0;

					if (info->port_subtype == ACPI_DBG2_PORT_SUBTYPE_PL011)
						arch_acpi_get_uart_pl011(uart);
					else if (info->port_subtype == ACPI_DBG2_PORT_SUBTYPE_16550)
						arch_acpi_get_uart_8250(uart);

					dprintf("discovered uart from dbg2 acpi: base=%lx\n", uart.regs.start);
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

		acpi_apic *desc = (acpi_apic*)(madt + 1);
		while (desc != (acpi_apic*)((char*)madt + madt->header.length)) {
			if (desc->type == ACPI_MADT_GIC_INTERFACE) {
				acpi_gic_interface *acpi_gicc = (acpi_gic_interface*)desc;
				if (acpi_gicc->cpu_interface_num == 0)
					gicc_base = acpi_gicc->base_address;

				platform_cpu_info* cpu = NULL;
				arch_smp_register_cpu(&cpu);
				if (cpu == NULL)
					continue;
				cpu->id = acpi_gicc->cpu_interface_num;
				cpu->mpidr = acpi_gicc->mpidr;

				// Firmware may describe the redistributors per-CPU here
				// instead of via a GICR structure. The frames are
				// contiguous, so the lowest base wins.
				if (acpi_gicc->gicr_address != 0
					&& (gicr_base == 0 || acpi_gicc->gicr_address < gicr_base)) {
					gicr_base = acpi_gicc->gicr_address;
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
