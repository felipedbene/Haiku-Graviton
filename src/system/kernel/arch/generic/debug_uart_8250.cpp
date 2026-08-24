/*
 * Copyright (c) 2008 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */


#include <arch/generic/debug_uart_8250.h>
#include <debug.h>
#include <new>


DebugUART8250::DebugUART8250(addr_t base, int64 clock, int8 regShift)
	:
	DebugUART(base, clock),
	fRegShift(regShift),
	fTxBatch(1)
{
}


DebugUART8250::~DebugUART8250()
{
}


#define UART_RHR    0
#define UART_THR    0
#define UART_DLL    0
#define UART_IER    1
#define UART_DLH    1
#define UART_IIR    2
#define UART_FCR    2
#define UART_EFR    2
#define UART_LCR    3
#define UART_MCR    4
#define UART_LSR    5
#define UART_MSR    6
#define UART_TCR    6
#define UART_SPR    7
#define UART_TLR    7

#define LCR_8N1		0x03

#define FCR_FIFO_EN	0x01	/* Fifo enable */
#define FCR_RXSR	0x02	/* Receiver soft reset */
#define FCR_TXSR	0x04	/* Transmitter soft reset */

#define MCR_DTR		0x01
#define MCR_RTS		0x02
#define MCR_DMA_EN	0x04
#define MCR_TX_DFR	0x08

#define LCR_WLS_MSK	0x03	/* character length select mask */
#define LCR_WLS_5	0x00	/* 5 bit character length */
#define LCR_WLS_6	0x01	/* 6 bit character length */
#define LCR_WLS_7	0x02	/* 7 bit character length */
#define LCR_WLS_8	0x03	/* 8 bit character length */
#define LCR_STB		0x04	/* Number of stop Bits, off = 1, on = 1.5 or 2) */
#define LCR_PEN		0x08	/* Parity eneble */
#define LCR_EPS		0x10	/* Even Parity Select */
#define LCR_STKP	0x20	/* Stick Parity */
#define LCR_SBRK	0x40	/* Set Break */
#define LCR_BKSE	0x80	/* Bank select enable */

#define LSR_DR		0x01	/* Data ready */
#define LSR_OE		0x02	/* Overrun */
#define LSR_PE		0x04	/* Parity error */
#define LSR_FE		0x08	/* Framing error */
#define LSR_BI		0x10	/* Break */
#define LSR_THRE	0x20	/* Xmit holding register empty */
#define LSR_TEMT	0x40	/* Xmitter empty */
#define LSR_ERR		0x80	/* Error */

/* Bits 7:6 of IIR -- the read side of the FCR address -- are the only report the
   chip makes about its own FIFO: 0b11 is FIFO mode, 0b10 is a FIFO the original
   16550 erratum makes unusable, 0b00 is no FIFO at all. */
#define IIR_FIFO_MASK		0xc0
#define IIR_FIFO_ENABLED	0xc0

/* Largest transmit batch this driver will accept, from the probe or from a
   setting. A 16550A holds 16 bytes; there is nothing to gain from writing more
   than one FIFO between status polls, and the cost of guessing high is dropped
   characters. */
#define MAX_TX_BATCH		16


// The base class assumes a fixed 32-bit register stride on ARM, which only
// holds for the SoC UARTs it was written for. Where the register spacing is
// actually known -- from the ACPI SPCR/DBG2 address structure or the device
// tree "reg-shift" property -- honour it, or the accesses land outside the
// register block entirely.
void
DebugUART8250::Out8(int reg, uint8 value)
{
	if (fRegShift < 0) {
		DebugUART::Out8(reg, value);
		return;
	}

	*(volatile uint8*)(Base() + (reg << fRegShift)) = value;
}


uint8
DebugUART8250::In8(int reg)
{
	if (fRegShift < 0)
		return DebugUART::In8(reg);

	return *(volatile uint8*)(Base() + (reg << fRegShift));
}


void
DebugUART8250::InitPort(uint32 baud)
{
	Disable();

	uint16 baudDivisor = Clock() / (16 * baud);

	// Write standard uart settings
	Out8(UART_LCR, LCR_8N1);
	// 8N1
	Out8(UART_IER, 0);
	// Disable interrupt
	Out8(UART_FCR, 0);
	// Disable FIFO
	Out8(UART_MCR, MCR_DTR | MCR_RTS);
	// DTR / RTS

	// Gain access to, and program baud divisor
	unsigned char buffer = In8(UART_LCR);
	Out8(UART_LCR, buffer | LCR_BKSE);
	Out8(UART_DLL, baudDivisor & 0xff);
	Out8(UART_DLH, (baudDivisor >> 8) & 0xff);
	Out8(UART_LCR, buffer & ~LCR_BKSE);

//	Out8(UART_MDR1, 0); // UART 16x mode
//	Out8(UART_LCR, 0xBF); // config mode B
//	Out8(UART_EFR, (1<<7)|(1<<6)); // hw flow control
//	Out8(UART_LCR, LCR_8N1); // operational mode

	Enable();
}


void
DebugUART8250::InitEarly()
{
}


void
DebugUART8250::Init()
{
}


// Wait for room in the transmit holding register, not for the transmitter to
// run dry. LSR_TEMT (0x40) is only set once the shift register has emptied too,
// so waiting on it serialises every character against the full bit time of its
// predecessor and leaves the FIFO permanently unused: one character in flight at
// a time, no matter how deep the hardware queue is. LSR_THRE (0x20) is the "you
// may write another byte" flag, which is what this loop wants.
//
// On a 16550 with the FIFO enabled, THRE means the whole FIFO is available, so
// this is the difference between paying a character time per character and
// paying one per FIFO-full. dprintf() is written a byte at a time and
// synchronously from a spinlock, which makes that cost a barrier in front of
// every kernel diagnostic rather than a detail of the console.
int
DebugUART8250::PutChar(char c)
{
	// wait for room in the tx holding register / fifo
	int32 timeout = 256 * 1024;
	while (!(In8(UART_LSR) & LSR_THRE)) {
		if (--timeout == 0)
			return -1;
	}

	Out8(UART_THR, c);
	return 0;
}


/*!	Set the number of bytes PutChars() may write between two reads of LSR.

	Clamped to 1..MAX_TX_BATCH. 1 means one poll per byte, which is what PutChar()
	does and therefore the behaviour this class has always had; it is the default,
	and the only value that is safe without knowing the FIFO is on.
*/
void
DebugUART8250::SetTxBatch(uint8 batch)
{
	if (batch < 1)
		batch = 1;
	else if (batch > MAX_TX_BATCH)
		batch = MAX_TX_BATCH;

	fTxBatch = batch;
}


/*!	Ask the chip whether its transmit FIFO is usable and pick a batch depth.

	Returns the depth installed: \a limit if the FIFO reports itself enabled,
	otherwise 1.

	This has to be a runtime question, not a build-time constant, because the two
	platforms this one class serves disagree and neither answer is available from
	the source. InitPort() writes FCR = 0, which disables the FIFO -- but on a UART
	discovered through ACPI the boot loader sets gUARTSkipInit and InitPort() is
	never called at all, so the FIFO is in whatever state the firmware left it.
	On a device tree without a "skip-init" property InitPort() does run, and the
	FIFO is then definitively off.

	Anything other than a clean "FIFO mode" report falls back to 1, including
	values that should not occur. Guessing high costs dropped characters in exactly
	the output being relied on to diagnose the machine, so an ambiguous answer is
	treated as a no.

	Note the deliberate asymmetry: this reports what is *enabled*, never how
	*deep* it is. IIR carries no depth field, a 16550A is 16 bytes deep and later
	and emulated parts are various, so the caller is expected to pass a limit it
	is willing to defend rather than the largest depth that might be there.
*/
uint8
DebugUART8250::ProbeTxBatch(uint8 limit)
{
	uint8 batch = 1;
	if ((In8(UART_IIR) & IIR_FIFO_MASK) == IIR_FIFO_ENABLED)
		batch = limit;

	SetTxBatch(batch);
	return fTxBatch;
}


/*!	Write \a length characters, polling LSR once per batch rather than per byte.

	The invariant this rests on: at every write that is not directly preceded by
	an observation of LSR_THRE, at most fTxBatch - 1 bytes have been written since
	the last such observation, within this same call. On a 16550 with the FIFO
	enabled THRE says the transmit FIFO is *empty*, so any fTxBatch no larger than
	the true FIFO depth cannot overflow it.

	All of the batch state is local to the call; nothing survives it. That is the
	point rather than an incidental style choice. A count carried in the object
	would have to stay correct across a KDL entry, across a panic raised from
	inside this function, and across firmware that re-initialises the port behind
	our back -- and a stale count overflows the FIFO and corrupts precisely the
	output being used to find out what went wrong.

	Caveat, stated rather than hidden: two writers running concurrently without
	serialising would each read THRE, each believe the whole FIFO was theirs, and
	together could overflow it. Such writers already interleave their characters
	today, so this widens a failure mode that exists rather than creating one, and
	the kernel debug output path serialises on a spinlock.
*/
void
DebugUART8250::PutChars(const char* string, size_t length)
{
	const size_t batch = fTxBatch;
	size_t written = 0;

	while (written < length) {
		// wait for room in the tx holding register / fifo
		int32 timeout = 256 * 1024;
		while (!(In8(UART_LSR) & LSR_THRE)) {
			if (--timeout == 0)
				return;
		}

		size_t chunk = length - written;
		if (chunk > batch)
			chunk = batch;

		for (size_t i = 0; i < chunk; i++)
			Out8(UART_THR, string[written + i]);

		written += chunk;
	}
}


/* returns -1 if no data available */
int
DebugUART8250::GetChar(bool wait)
{
	if (wait) {
		while (!(In8(UART_LSR) & LSR_DR));
			// wait for data to show up in the rx fifo
	} else {
		if (!(In8(UART_LSR) & LSR_DR))
			return -1;
	}
	return In8(UART_RHR);
}


void
DebugUART8250::FlushTx()
{
	// LSR_TEMT and not LSR_THRE on purpose: a flush is the one caller that does
	// want the shift register drained as well, so that the last byte has really
	// left the wire before, say, a reset or a jump to the kernel.
	while (!(In8(UART_LSR) & LSR_TEMT));
		// wait for the last char to get out
}


void
DebugUART8250::FlushRx()
{
	// empty the rx fifo
	while (In8(UART_LSR) & LSR_DR) {
		volatile char c = In8(UART_RHR);
		(void)c;
	}
}


DebugUART8250*
arch_get_uart_8250(addr_t base, int64 clock, int8 regShift)
{
	static char buffer[sizeof(DebugUART8250)];
	DebugUART8250* uart = new(buffer) DebugUART8250(base, clock, regShift);
	return uart;
}

