/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * ARM PrimeCell PL061 GPIO controller (ACPI _HID "ARMH0061"), driven as a
 * source of GPIO-signalled ACPI events.
 */


/*!	Why this driver exists at all.

	On a PC the power button is an ACPI fixed feature: pressing it sets a bit in
	the PM1 event block, the SCI fires, and ACPICA dispatches a fixed event.
	Hardware-reduced platforms -- which is every ARM server, and which is what
	HW_REDUCED_ACPI in the FADT announces -- have no PM1 event block at all.
	There, the button is a *control-method* device (PNP0C0C) whose _Exx method
	is run when a particular GPIO pin toggles, and the method's AML issues a
	Notify() that the driver of the button device receives. The chain is:

	  pin toggles -> GPIO controller raises its interrupt
	    -> this driver runs the pin's _Exx method
	      -> the AML in that method does Notify(PWRB, 0x80)
	        -> acpi_button's notify handler marks /dev/power/button/power
	          -> power_daemon reads it and asks the registrar to shut down

	Haiku implemented none of the first two steps, so the middle of that chain
	was missing and the last three could never happen. That is a real problem on
	a virtual machine, because the hypervisor's "please shut down" request is
	exactly this event: an AWS Nitro instance being stopped is sent the
	equivalent of one power-button press and then hard powered off a few minutes
	later, so a guest that cannot receive the event never shuts down cleanly and
	loses every unflushed write, every time.

	Nothing here is specific to one machine, and nothing may be hardcoded: the
	register base, the interrupt number, the pin numbers and the method names
	all differ between instance types of the same family, never mind between
	vendors. Everything is read from the namespace -- _CRS for the registers and
	the interrupt, _AEI for the pins -- which is also what makes this the
	generic PL061 event driver rather than a workaround.

	Deliberately not implemented: GPIO as a general resource. Haiku has no GPIO
	framework to plug into, and inventing one is not needed to receive an ACPI
	event. This driver only ever touches pins that firmware listed in _AEI, and
	only ever reports them to firmware's own methods.
*/


#include <ACPI.h>
#include <KernelExport.h>
#include <dpc.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acpi.h"


#define PL061_MODULE_NAME	"drivers/power/pl061_acpi_event/driver_v1"

//#define TRACE_PL061
#ifdef TRACE_PL061
#	define TRACE(x...)	dprintf("pl061: " x)
#else
#	define TRACE(x...)
#endif
#define INFO(x...)	dprintf("pl061: " x)
#define ERROR(x...)	dprintf("pl061: " x)


/*	PL061 register offsets (ARM DDI 0190B). Every register is eight bits wide,
	one bit per pin, and the TRM permits byte access -- which is what Linux's
	driver does on this same hardware, so it is the access size known to work.
*/
#define PL061_GPIODIR	0x400	// direction, 0 = input
#define PL061_GPIOIS	0x404	// interrupt sense, 0 = edge, 1 = level
#define PL061_GPIOIBE	0x408	// both edges, 1 = both
#define PL061_GPIOIEV	0x40c	// event, 1 = rising/high, 0 = falling/low
#define PL061_GPIOIE	0x410	// interrupt enable, 1 = enabled
#define PL061_GPIOMIS	0x418	// masked interrupt status
#define PL061_GPIOIC	0x41c	// interrupt clear, write 1 to clear

#define PL061_PIN_COUNT	8


static device_manager_info* sDeviceManager;
static acpi_module_info* sAcpi;
static dpc_module_info* sDPC;


struct pl061_event {
	uint8	pin;
	char	method[8];	// "_E03", "_L04", ...
};


struct pl061_info {
	acpi_handle		handle;
	area_id			registersArea;
	uint8* volatile	registers;
	int32			irq;
	bool			irqInstalled;
	void*			dpcQueue;

	// Bit mask of pins whose interrupt has fired and not yet been dispatched.
	int32			pending;

	pl061_event		events[PL061_PIN_COUNT];
	uint32			eventCount;
};


static inline uint8
pl061_read(pl061_info* bus, uint32 offset)
{
	return *(volatile uint8*)(bus->registers + offset);
}


static inline void
pl061_write(pl061_info* bus, uint32 offset, uint8 value)
{
	*(volatile uint8*)(bus->registers + offset) = value;
}


static inline void
pl061_set_bit(pl061_info* bus, uint32 offset, uint8 mask, bool set)
{
	uint8 value = pl061_read(bus, offset);
	if (set)
		value |= mask;
	else
		value &= ~mask;
	pl061_write(bus, offset, value);
}


//	#pragma mark - resource parsing


struct pl061_crs {
	uint64	base;
	uint64	length;
	int32	irq;
};


static acpi_status
pl061_parse_crs(ACPI_RESOURCE* res, void* context)
{
	pl061_crs* crs = (pl061_crs*)context;

	switch (res->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			crs->base = res->Data.FixedMemory32.Address;
			crs->length = res->Data.FixedMemory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			crs->base = res->Data.Memory32.Minimum;
			crs->length = res->Data.Memory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_ADDRESS64:
			crs->base = res->Data.Address64.Address.Minimum;
			crs->length = res->Data.Address64.Address.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (res->Data.ExtendedIrq.InterruptCount > 0)
				crs->irq = res->Data.ExtendedIrq.Interrupts[0];
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (res->Data.Irq.InterruptCount > 0)
				crs->irq = res->Data.Irq.Interrupts[0];
			break;
		default:
			break;
	}

	return B_OK;
}


static acpi_status
pl061_parse_aei(ACPI_RESOURCE* res, void* context)
{
	pl061_info* bus = (pl061_info*)context;

	if (res->Type != ACPI_RESOURCE_TYPE_GPIO
		|| res->Data.Gpio.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT) {
		return B_OK;
	}

	// PinTableLength counts entries, not bytes (see AcpiRsCalculate*, which
	// multiplies it by two to get the AML byte count).
	for (uint16 i = 0; i < res->Data.Gpio.PinTableLength; i++) {
		uint16 pin = res->Data.Gpio.PinTable[i];
		if (pin >= PL061_PIN_COUNT) {
			ERROR("_AEI lists pin %u, which this controller does not have\n",
				pin);
			continue;
		}
		if (bus->eventCount >= PL061_PIN_COUNT)
			return AE_CTRL_TERMINATE;

		const bool edge = res->Data.Gpio.Triggering == ACPI_EDGE_SENSITIVE;

		// ACPI 6.x 5.6.5: a GPIO-signalled event is named _Exx for an
		// edge-triggered pin and _Lxx for a level-triggered one, xx being the
		// pin number in hex. Firmware is not always consistent about which
		// spelling it uses, so accept whichever one is actually present rather
		// than trusting the descriptor. This has to be settled now, by looking
		// the name up: probing it by evaluation would *press the button*.
		pl061_event& event = bus->events[bus->eventCount];
		acpi_handle methodHandle;
		snprintf(event.method, sizeof(event.method), "_%c%02X",
			edge ? 'E' : 'L', pin);
		if (sAcpi->get_handle(bus->handle, event.method, &methodHandle)
				!= B_OK) {
			snprintf(event.method, sizeof(event.method), "_%c%02X",
				edge ? 'L' : 'E', pin);
			if (sAcpi->get_handle(bus->handle, event.method, &methodHandle)
					!= B_OK) {
				ERROR("_AEI lists pin %u but there is no _E%02X or _L%02X "
					"method to run for it; ignoring the pin\n", pin, pin, pin);
				continue;
			}
		}

		// Configure the pin before it is armed. Note the order below in
		// pl061_configure(): the latched state is cleared last but one and the
		// enable comes last, because a pin left latched by firmware would
		// otherwise fire the moment it is enabled -- which, for the power
		// button, means shutting the machine down as it finishes booting.
		event.pin = (uint8)pin;
		bus->eventCount++;

		INFO("pin %u -> %s (%s triggered, active %s)\n", pin, event.method,
			edge ? "edge" : "level",
			res->Data.Gpio.Polarity == ACPI_ACTIVE_LOW ? "low"
				: (res->Data.Gpio.Polarity == ACPI_ACTIVE_BOTH ? "both"
					: "high"));
	}

	return B_OK;
}


static void
pl061_configure(pl061_info* bus, ACPI_RESOURCE_GPIO* gpio, uint8 pin)
{
	const uint8 mask = 1 << pin;

	// An event pin is an input.
	pl061_set_bit(bus, PL061_GPIODIR, mask, false);

	pl061_set_bit(bus, PL061_GPIOIS, mask,
		gpio->Triggering != ACPI_EDGE_SENSITIVE);
	pl061_set_bit(bus, PL061_GPIOIBE, mask,
		gpio->Polarity == ACPI_ACTIVE_BOTH);
	pl061_set_bit(bus, PL061_GPIOIEV, mask,
		gpio->Polarity != ACPI_ACTIVE_LOW);
}


//	#pragma mark - interrupt and event dispatch


static void
pl061_dispatch_events(void* data)
{
	pl061_info* bus = (pl061_info*)data;

	// Take the whole pending mask at once: a second interrupt may queue another
	// DPC while this one runs, and the second must then find nothing to do
	// rather than run the same method twice.
	const uint32 pending = (uint32)atomic_and(&bus->pending, 0);
	if (pending == 0)
		return;

	for (uint32 i = 0; i < bus->eventCount; i++) {
		pl061_event& event = bus->events[i];
		if ((pending & (1 << event.pin)) == 0)
			continue;

		// Log this at INFO, not TRACE. A GPIO event here is rare -- on a Nitro
		// guest it is the hypervisor's stop/reboot request arriving as the
		// platform power button -- so it costs nothing to record, and it is the
		// only externally visible sign that the inbound reset path fired at all.
		// Without it the whole chain is silent, which is exactly what made this
		// path impossible to tell apart from a no-op when verifying it.
		INFO("pin %u signalled; running %s\n", event.pin, event.method);
		status_t status = sAcpi->evaluate_method(bus->handle, event.method,
			NULL, NULL);
		if (status != B_OK) {
			ERROR("%s failed: %s\n", event.method, strerror(status));
		}
	}
}


static int32
pl061_interrupt_handler(void* data)
{
	pl061_info* bus = (pl061_info*)data;

	const uint8 status = pl061_read(bus, PL061_GPIOMIS);
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	// Acknowledge in the controller now, before the event is dispatched. The
	// _Exx method runs in a DPC and can take milliseconds; an edge arriving
	// during that window has to be able to latch again rather than be lost, and
	// the GIC sees this as a level interrupt, so leaving it asserted here would
	// re-enter this handler forever.
	pl061_write(bus, PL061_GPIOIC, status);

	atomic_or(&bus->pending, status);

	// AML cannot be evaluated from interrupt context, so the work is deferred.
	// queue_dpc() is documented to be callable from an interrupt handler.
	if (sDPC->queue_dpc(bus->dpcQueue, pl061_dispatch_events, bus) != B_OK)
		ERROR("could not queue the event dispatch\n");

	return B_HANDLED_INTERRUPT;
}


//	#pragma mark - driver module API


static float
pl061_support(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK) {
		return -1;
	}
	if (strcmp(bus, "acpi") != 0)
		return 0.0;

	uint32 deviceType;
	if (sDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM,
			&deviceType, false) != B_OK || deviceType != ACPI_TYPE_DEVICE) {
		return 0.0;
	}

	const char* hid;
	if (sDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid,
			false) != B_OK || strcmp(hid, "ARMH0061") != 0) {
		return 0.0;
	}

	return 0.6;
}


static status_t
pl061_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "PL061 GPIO ACPI events" }},

		// This driver publishes no device of its own -- it exists to run
		// firmware's event methods -- so without this the device manager would
		// uninitialize it again as unused the moment registration finished.
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
		{ NULL }
	};

	return sDeviceManager->register_node(parent, PL061_MODULE_NAME, attrs,
		NULL, NULL);
}


/*!	Second _AEI pass: configure the pins this driver accepted.

	Splitting configuration out of pl061_parse_aei() keeps the registers
	untouched until the mapping has succeeded, so a controller whose _CRS we
	could not map is never half-programmed.
*/
static acpi_status
pl061_configure_aei(ACPI_RESOURCE* res, void* context)
{
	pl061_info* bus = (pl061_info*)context;

	if (res->Type != ACPI_RESOURCE_TYPE_GPIO
		|| res->Data.Gpio.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT) {
		return B_OK;
	}

	for (uint16 i = 0; i < res->Data.Gpio.PinTableLength; i++) {
		const uint16 pin = res->Data.Gpio.PinTable[i];
		for (uint32 j = 0; j < bus->eventCount; j++) {
			if (bus->events[j].pin == pin)
				pl061_configure(bus, &res->Data.Gpio, (uint8)pin);
		}
	}

	return B_OK;
}


static status_t
pl061_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	if (parent == NULL)
		return B_ERROR;

	const char* path;
	status_t status = sDeviceManager->get_attr_string(parent,
		ACPI_DEVICE_PATH_ITEM, &path, false);
	sDeviceManager->put_node(parent);
	if (status != B_OK) {
		ERROR("no ACPI path attribute\n");
		return status;
	}

	pl061_info* bus = new(std::nothrow) pl061_info;
	if (bus == NULL)
		return B_NO_MEMORY;
	memset(bus, 0, sizeof(*bus));
	bus->registersArea = -1;
	bus->irq = -1;

	status = sAcpi->get_handle(NULL, path, &bus->handle);
	if (status != B_OK) {
		ERROR("cannot resolve %s\n", path);
		delete bus;
		return status;
	}

	// _CRS: where the registers are and which interrupt the controller drives.
	pl061_crs crs = { 0, 0, -1 };
	status = sAcpi->walk_resources(bus->handle, (char*)"_CRS",
		pl061_parse_crs, &crs);
	if (status != B_OK || crs.base == 0 || crs.length == 0 || crs.irq < 0) {
		ERROR("%s has no usable _CRS (base %#" B_PRIx64 ", length %#" B_PRIx64
			", irq %" B_PRId32 ")\n", path, crs.base, crs.length, crs.irq);
		delete bus;
		return B_ERROR;
	}
	bus->irq = crs.irq;

	// _AEI: which pins firmware wants events for. A PL061 with no _AEI is a
	// plain GPIO controller with nothing here to offer it, so leave it alone
	// rather than claiming it.
	status = sAcpi->walk_resources(bus->handle, (char*)"_AEI",
		pl061_parse_aei, bus);
	if (status != B_OK || bus->eventCount == 0) {
		TRACE("%s declares no ACPI event pins\n", path);
		delete bus;
		return B_ERROR;
	}

	bus->registersArea = map_physical_memory("PL061 GPIO registers",
		(phys_addr_t)crs.base, (size_t)crs.length, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&bus->registers);
	if (bus->registersArea < 0) {
		ERROR("cannot map registers at %#" B_PRIx64 "\n", crs.base);
		status = bus->registersArea;
		delete bus;
		return status;
	}

	status = sDPC->new_dpc_queue(&bus->dpcQueue, "pl061 events",
		B_NORMAL_PRIORITY);
	if (status != B_OK) {
		ERROR("cannot create a DPC queue: %s\n", strerror(status));
		delete_area(bus->registersArea);
		delete bus;
		return status;
	}

	uint8 eventMask = 0;
	for (uint32 i = 0; i < bus->eventCount; i++)
		eventMask |= 1 << bus->events[i].pin;

	// Mask first: whatever state firmware left the controller in, no pin of
	// ours may deliver an interrupt until it is fully configured.
	pl061_set_bit(bus, PL061_GPIOIE, eventMask, false);

	sAcpi->walk_resources(bus->handle, (char*)"_AEI", pl061_configure_aei, bus);

	// Discard anything latched during boot, then arm. In this order: an edge
	// that firmware left pending is not an event this Haiku ever saw, and
	// acting on it would shut the machine down moments after it came up.
	pl061_write(bus, PL061_GPIOIC, eventMask);

	status = install_io_interrupt_handler(bus->irq, pl061_interrupt_handler,
		bus, 0);
	if (status != B_OK) {
		ERROR("cannot install a handler for irq %" B_PRId32 ": %s\n", bus->irq,
			strerror(status));
		sDPC->delete_dpc_queue(bus->dpcQueue);
		delete_area(bus->registersArea);
		delete bus;
		return status;
	}
	bus->irqInstalled = true;

	pl061_set_bit(bus, PL061_GPIOIE, eventMask, true);

	INFO("%s: %" B_PRIu32 " event pin(s) armed on irq %" B_PRId32 "\n", path,
		bus->eventCount, bus->irq);

	*_cookie = bus;
	return B_OK;
}


static void
pl061_uninit_driver(void* cookie)
{
	pl061_info* bus = (pl061_info*)cookie;
	if (bus == NULL)
		return;

	uint8 eventMask = 0;
	for (uint32 i = 0; i < bus->eventCount; i++)
		eventMask |= 1 << bus->events[i].pin;

	if (bus->registers != NULL)
		pl061_set_bit(bus, PL061_GPIOIE, eventMask, false);

	if (bus->irqInstalled) {
		remove_io_interrupt_handler(bus->irq, pl061_interrupt_handler, bus);
	}

	// After the interrupt source is gone, so nothing can queue onto it.
	if (bus->dpcQueue != NULL)
		sDPC->delete_dpc_queue(bus->dpcQueue);

	if (bus->registersArea >= 0)
		delete_area(bus->registersArea);

	delete bus;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{ B_ACPI_MODULE_NAME, (module_info**)&sAcpi },
	{ B_DPC_MODULE_NAME, (module_info**)&sDPC },
	{}
};


static driver_module_info sPL061DriverModule = {
	{
		PL061_MODULE_NAME,
		0,
		NULL
	},

	pl061_support,
	pl061_register_device,
	pl061_init_driver,
	pl061_uninit_driver,
	NULL,	// register_child_devices
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sPL061DriverModule,
	NULL
};
