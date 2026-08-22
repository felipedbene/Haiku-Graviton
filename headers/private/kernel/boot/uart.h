/*
 * Copyright 2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_BOOT_UART_H
#define KERNEL_BOOT_UART_H


#include <boot/addr_range.h>
#include <SupportDefs.h>


// Distance between consecutive registers, as log2 of the byte stride: 0 for a
// byte-packed register block, 2 for the 32-bit spaced one most SoC UARTs use.
// UART_REG_SHIFT_UNSET means the boot loader could not tell, and the driver
// keeps whatever its platform has always assumed.
#define UART_REG_SHIFT_UNSET	((int8)-1)


typedef struct {
	char kind[32];
	addr_range regs;
	uint32 irq;
	int64 clock;
	int8 reg_shift;
} __attribute__((packed)) uart_info;


#endif /* KERNEL_BOOT_UART_H */
