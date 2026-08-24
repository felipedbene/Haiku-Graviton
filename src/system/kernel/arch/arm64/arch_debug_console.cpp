/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <arch/debug_console.h>
#include <arch/generic/debug_uart.h>
#include <arch/generic/debug_uart_8250.h>
// #include <arch/arm/arch_uart_8250_omap.h>
#include <arch/arm/arch_uart_pl011.h>
#include <arch/arm64/arch_uart_linflex.h>
#include <arch/arm64/arch_uart_samsung.h>
#include <boot/kernel_args.h>
#include <debug.h>
#include <driver_settings.h>
#include <kernel.h>
#include <vm/vm.h>
#include <stdlib.h>
#include <string.h>


static DebugUART *sArchDebugUART = NULL;

// The same object as sArchDebugUART, kept also as its concrete type when the
// console happens to be an 8250: the transmit batching controls are specific to
// that part and do not belong on the abstract interface.
static DebugUART8250 *sArchDebugUART8250 = NULL;

// Batch depth to install when -- and only when -- the chip reports at runtime
// that its transmit FIFO is enabled. Deliberately well below the 16 bytes a
// 16550A holds, because the register that reports "FIFO on" does not report how
// deep it is: this is a depth every 16550-compatible FIFO has, not the largest
// one that might be there. The serial_debug_tx_batch setting overrides it.
static const uint8 kProbedTxBatch = 4;


void
arch_debug_remove_interrupt_handler(uint32 line)
{
}


int
arch_debug_blue_screen_try_getchar(void)
{
	return arch_debug_serial_try_getchar();
}


char
arch_debug_blue_screen_getchar(void)
{
	return arch_debug_serial_getchar();
}


/*!	Returns the next character, or -1 if none is waiting.

	This used to return arch_debug_serial_getchar(), whose return type is char.
	DebugUART::GetChar(false) reports "nothing waiting" as -1, and plain char is
	*unsigned* on AArch64, so that -1 became 255 and this function could never
	return a negative value. kgetc() tests `c >= 0`, so inside KDL it accepted a
	continuous stream of 0xFF bytes instead of waiting for input: every command
	line filled with garbage and the debugger was effectively unusable for typed
	commands on this architecture.
*/
int
arch_debug_serial_try_getchar(void)
{
	if (sArchDebugUART == NULL)
		return -1;

	return sArchDebugUART->GetChar(false);
}


/*!	Blocks until a character arrives.

	Also previously wrong: it passed wait=false, so it returned immediately with
	(char)-1 == 255 when the receive FIFO was empty, despite being the blocking
	half of the interface.
*/
char
arch_debug_serial_getchar(void)
{
	if (sArchDebugUART == NULL)
		return '\0';

	int c = sArchDebugUART->GetChar(true);
	if (c < 0)
		return '\0';

	return (char)c;
}


void
arch_debug_serial_putchar(const char c)
{
	if (sArchDebugUART == NULL)
		return;

	sArchDebugUART->PutChar(c);
}


/*!	Writes \a s to the debug console, translating '\n' into "\r\n".

	Hands whole runs to PutChars() rather than looping over PutChar(). The
	translation is exactly what it was -- '\n' becomes "\r\n", a bare '\r' is
	dropped, everything else passes through -- and on a driver that does not
	override PutChars() the resulting sequence of PutChar() calls is identical
	too. What changes is that the driver can now see more than one character at a
	time, which is the only way it can amortise a transmit wait; a per-character
	loop discards that information here, where the driver cannot get it back.

	The buffer is small and on the stack on purpose. This runs from panic() and
	from KDL, so it must not allocate and must not depend on any state outside
	this call.
*/
void
arch_debug_serial_puts(const char *s)
{
	if (sArchDebugUART == NULL)
		return;

	char buffer[128];
	size_t used = 0;

	while (*s != '\0') {
		char ch = *s++;

		if (ch == '\r')
			continue;

		// '\n' expands to two characters, so leave room for both
		if (used + 2 > sizeof(buffer)) {
			sArchDebugUART->PutChars(buffer, used);
			used = 0;
		}

		if (ch == '\n')
			buffer[used++] = '\r';

		buffer[used++] = ch;
	}

	if (used > 0)
		sArchDebugUART->PutChars(buffer, used);
}


void
arch_debug_serial_early_boot_message(const char *string)
{
	// this function will only be called in fatal situations
	arch_debug_serial_puts(string);
}


status_t
arch_debug_console_init(kernel_args *args)
{
	if (strncmp(args->arch_args.uart.kind, UART_KIND_PL011,
		sizeof(args->arch_args.uart.kind)) == 0) {
		sArchDebugUART = arch_get_uart_pl011(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock);
	} else if (strncmp(args->arch_args.uart.kind, UART_KIND_LINFLEX,
		sizeof(args->arch_args.uart.kind)) == 0) {
		sArchDebugUART = arch_get_uart_linflex(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock);
	}/* else if (strncmp(args->arch_args.uart.kind, UART_KIND_8250_OMAP,
		sizeof(args->arch_args.uart.kind)) == 0) {
		sArchDebugUART = arch_get_uart_8250_omap(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock);
	}*/ else if (strncmp(args->arch_args.uart.kind, UART_KIND_8250,
		sizeof(args->arch_args.uart.kind)) == 0) {
		sArchDebugUART8250 = arch_get_uart_8250(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock, args->arch_args.uart.reg_shift);
		sArchDebugUART = sArchDebugUART8250;
	} else if (strncmp(args->arch_args.uart.kind, UART_KIND_SAMSUNG,
		sizeof(args->arch_args.uart.kind)) == 0) {
		sArchDebugUART = arch_get_uart_samsung(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock);
	}

	// Oh well.
	if (sArchDebugUART == NULL)
		return B_ERROR;

	sArchDebugUART->InitEarly();

	return B_OK;
}



/*!	Picks the transmit batch depth for an 8250 console.

	Nothing here needs a thread: this runs from debug_init_post_settings(), which
	main.cpp calls well before thread_init(). The serial KDL listener lives in the
	generic debugger and is started from debug_init_post_modules() for that reason.

	Note what this means for anything printed earlier -- every early boot message,
	and every panic before this point: the depth is still 1 then, so that output is
	produced by exactly the code path it always was.
*/
status_t
arch_debug_console_init_settings(kernel_args *args)
{
	if (sArchDebugUART8250 == NULL)
		return B_OK;

	// Ask the chip whether its transmit FIFO is enabled. It answers 1 -- a status
	// poll per byte -- unless it says yes.
	uint8 batch = sArchDebugUART8250->ProbeTxBatch(kProbedTxBatch);
	const bool probedFifo = batch > 1;

	// An explicit setting wins over the probe, for two reasons. It makes depth 1
	// -- the behaviour this console has always had -- reachable as a control
	// without rebuilding, and it makes a depth A/B a matter of two reboots of one
	// image rather than two images. Out-of-range values are clamped, not rejected.
	void *handle = load_driver_settings("kernel");
	if (handle != NULL) {
		const char *value = get_driver_parameter(handle, "serial_debug_tx_batch",
			NULL, NULL);
		if (value != NULL) {
			long number = strtol(value, NULL, 0);
			if (number < 0)
				number = 0;
			else if (number > 255)
				number = 255;

			sArchDebugUART8250->SetTxBatch((uint8)number);
			batch = sArchDebugUART8250->TxBatch();
		}

		unload_driver_settings(handle);
	}

	// Say which depth is in force. A change to how the console writes bytes is
	// invisible in the output it produces, so without this line there is no way
	// to tell from a boot log whether a kernel has this code at all, let alone
	// what depth it settled on -- and "the log looks the same" is then
	// indistinguishable from "the setting was ignored".
	dprintf("uart 8250: tx fifo %sreported enabled, tx batch %u byte%s per "
		"status poll\n", probedFifo ? "" : "not ", (unsigned)batch,
		batch == 1 ? "" : "s");

	return B_OK;
}
