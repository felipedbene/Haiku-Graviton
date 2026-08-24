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



status_t
arch_debug_console_init_settings(kernel_args *args)
{
	// Nothing to do here, and in particular nothing that needs a thread: this
	// runs from debug_init_post_settings(), which main.cpp calls well before
	// thread_init(). The serial KDL listener lives in the generic debugger and
	// is started from debug_init_post_modules() for that reason.
	return B_OK;
}
