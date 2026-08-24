/*
 * Copyright 2012 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		François Revol, revol@free.fr
 */
#ifndef _KERNEL_ARCH_DEBUG_UART_H
#define _KERNEL_ARCH_DEBUG_UART_H


#include <sys/types.h>

#include <SupportDefs.h>


class DebugUART {
public:
							DebugUART(addr_t base, int64 clock)
								: fBase(base),
								fClock(clock),
								fEnabled(true) {};
							~DebugUART() {};

	virtual	void			InitEarly() {};
	virtual	void			Init() {};
	virtual	void			InitPort(uint32 baud) {};

	virtual	void			Enable() { fEnabled = true; }
	virtual	void			Disable() { fEnabled = false; }

	virtual	int				PutChar(char c) = 0;

							// Write a run of characters at once.
							//
							// The default implementation walks the string
							// through PutChar(), so a driver that does not
							// override it behaves exactly as it did before this
							// entry point existed. A driver whose transmit flow
							// control is per-queue rather than per-character can
							// override it and amortise one wait over several
							// bytes.
							//
							// Callers holding more than one character should
							// prefer this over a PutChar() loop: the loop throws
							// away, at the call site, the only information that
							// would let the driver batch.
	virtual	void			PutChars(const char* string, size_t length);

	virtual	int				GetChar(bool wait) = 0;

	virtual	void			FlushTx() = 0;
	virtual	void			FlushRx() = 0;

			void			SetBase(addr_t base) { fBase = base; }
			addr_t			Base() const { return fBase; }
			int64			Clock() const { return fClock; }
			bool			Enabled() const { return fEnabled; }

protected:
	virtual	void			Out8(int reg, uint8 value);
	virtual	uint8			In8(int reg);
	virtual	void			Barrier();

private:
			addr_t			fBase;
			int64			fClock;
			bool			fEnabled;
};


#endif	/* _KERNEL_ARCH_DEBUG_UART_H */
