/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "rtc.h"

#include <KernelExport.h>

#include <boot/kernel_args.h>
#include <boot/stage2.h>

#include "efi_platform.h"


#define RTC_SECONDS_DAY			86400
#define RTC_EPOCH_JULIAN_DAY	2440588
	// January 1st, 1970


static uint64
efi_time_to_secs(const efi_time& time)
{
	int year = time.Year;
	int month = time.Month;

	// Reference: Fliegel, H. F. and van Flandern, T. C. (1968).
	// Communications of the ACM, Vol. 11, No. 10 (October, 1968).
	uint32 days = time.Day - 32075 - RTC_EPOCH_JULIAN_DAY
		+ 1461 * (year + 4800 + (month - 14) / 12) / 4
		+ 367 * (month - 2 - 12 * ((month - 14) / 12)) / 12
		- 3 * ((year + 4900 + (month - 14) / 12) / 100) / 4;

	return (uint64)days * RTC_SECONDS_DAY + time.Hour * 3600 + time.Minute * 60
		+ time.Second;
}


/*!	Hands the firmware's notion of the wall clock to the kernel.

	Architectures which have no real time clock of their own (arm64) depend on
	this: the only clock reachable on both QEMU's virt machine and on EC2 is
	the one behind the EFI runtime services, and those are unusable once the
	kernel has switched to its own page tables.
*/
void
rtc_init(void)
{
	efi_time time;
	if (kRuntimeServices->GetTime(&time, NULL) != EFI_SUCCESS) {
		dprintf("rtc: firmware provides no time, clock will start at the "
			"epoch\n");
		return;
	}

	// The fields are used as-is, i.e. treated like any other hardware clock
	// whose timezone the kernel learns about later on.
	gKernelArgs.platform_args.firmware_time = efi_time_to_secs(time);

	dprintf("rtc: firmware time is %04u-%02u-%02u %02u:%02u:%02u (%"
		B_PRIu64 ")\n", (unsigned)time.Year, (unsigned)time.Month,
		(unsigned)time.Day, (unsigned)time.Hour, (unsigned)time.Minute,
		(unsigned)time.Second, gKernelArgs.platform_args.firmware_time);
}
