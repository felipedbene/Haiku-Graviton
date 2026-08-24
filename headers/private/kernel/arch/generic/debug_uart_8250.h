/*
 * Copyright 2012-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		François Revol, revol@free.fr
 *		Alexander von Gluck IV, kallisti5@unixzen.com
 */
#ifndef _KERNEL_ARCH_DEBUG_UART_8250_H
#define _KERNEL_ARCH_DEBUG_UART_8250_H


#include <sys/types.h>

#include <SupportDefs.h>

#include <boot/uart.h>

#include "debug_uart.h"


#define UART_KIND_8250 "8250"


class DebugUART8250 : public DebugUART {
public:
							DebugUART8250(addr_t base, int64 clock,
								int8 regShift = UART_REG_SHIFT_UNSET);
							~DebugUART8250();

			void			InitEarly();
			void			Init();
			void			InitPort(uint32 baud);

			int				PutChar(char c);
			void			PutChars(const char* string, size_t length);
			int				GetChar(bool wait);

			void			FlushTx();
			void			FlushRx();

							// How many bytes PutChars() may write between two
							// reads of the line status register. 1 -- one poll
							// per byte, identical to a PutChar() loop -- is the
							// default and the only safe value for a UART whose
							// FIFO state is unknown. See ProbeTxBatch().
			uint8			TxBatch() const { return fTxBatch; }
			void			SetTxBatch(uint8 batch);
			uint8			ProbeTxBatch(uint8 limit);

protected:
	virtual	void			Out8(int reg, uint8 value);
	virtual	uint8			In8(int reg);

private:
			int8			fRegShift;
			uint8			fTxBatch;
};


DebugUART8250* arch_get_uart_8250(addr_t base, int64 clock,
	int8 regShift = UART_REG_SHIFT_UNSET);


#endif /* _KERNEL_ARCH_DEBUG_UART_8250_H */
