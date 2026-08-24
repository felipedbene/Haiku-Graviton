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
#include <kernel.h>
#include <thread.h>
#include <vm/vm.h>
#include <string.h>


static DebugUART *sArchDebugUART = NULL;


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


void
arch_debug_serial_puts(const char *s)
{
	while (*s != '\0') {
		char ch = *s;
		if (ch == '\n') {
			arch_debug_serial_putchar('\r');
			arch_debug_serial_putchar('\n');
		} else if (ch != '\r')
			arch_debug_serial_putchar(ch);
		s++;
	}
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
		sArchDebugUART = arch_get_uart_8250(args->arch_args.uart.regs.start,
			args->arch_args.uart.clock, args->arch_args.uart.reg_shift);
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



/*!	Watches the debug serial line for a request to enter the kernel debugger.

	x86 reaches KDL on demand through its keyboard interrupt handler, which
	checks for Alt+SysReq+<key> and calls debug_emergency_key_pressed(). arm64
	had no equivalent, and a headless machine has no keyboard at all -- the EC2
	Graviton instances this runs on log "Search for bus_managers/ps2/v1 failed"
	and have no USB HID either. So there was no way whatsoever to enter KDL on
	demand: the debugger could only be reached by a panic.

	That matters most in exactly the situation KDL is wanted for. A machine whose
	userland has stopped making progress -- for instance every writer parked in
	the page writer's quota wait -- cannot be asked to run a command, and does not
	panic, so without this it cannot be inspected at all.

	A polling thread rather than a UART interrupt: it is far less invasive, and it
	still runs when userland is starved, because starved threads are blocked on a
	condition variable rather than consuming CPU.

	The trigger is the three-character sequence "kdl" rather than a single key,
	because a serial line has no modifier keys to qualify it with and stray input
	should not halt the machine. debug_emergency_key_pressed() applies the
	existing `emergency_keys` safemode setting, so this respects that too.
*/
static status_t
serial_debug_listener(void*)
{
	const char kTrigger[] = "kdl";
	const size_t kTriggerLength = sizeof(kTrigger) - 1;
	char recent[kTriggerLength];
	size_t have = 0;

	while (true) {
		int c = arch_debug_serial_try_getchar();
		if (c < 0) {
			snooze(100000);
			continue;
		}

		if (have == kTriggerLength) {
			memmove(recent, recent + 1, kTriggerLength - 1);
			have--;
		}
		recent[have++] = (char)c;

		if (have == kTriggerLength
			&& memcmp(recent, kTrigger, kTriggerLength) == 0) {
			have = 0;
			debug_emergency_key_pressed('d');
		}
	}

	return B_OK;
}


status_t
arch_debug_console_init_settings(kernel_args *args)
{
	if (sArchDebugUART == NULL)
		return B_OK;

	thread_id thread = spawn_kernel_thread(&serial_debug_listener,
		"serial debug listener", B_LOW_PRIORITY, NULL);
	if (thread >= 0)
		resume_thread(thread);

	return B_OK;
}
