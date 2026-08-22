/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <arch/real_time_clock.h>

#include <KernelExport.h>

#include <boot/kernel_args.h>
#include <real_time_clock.h>
#include <real_time_data.h>
#include <smp.h>


static uint64 sHardwareTime;
	// Wall clock in seconds as it was at sHardwareTimeUptime, 0 if unknown.
static bigtime_t sHardwareTimeUptime;


status_t
arch_rtc_init(kernel_args *args, struct real_time_data *data)
{
	// ARMv8 has no architectural real time clock, and the one clock reachable
	// on every platform we care about hangs off the EFI runtime services,
	// which are gone once the kernel runs on its own page tables. So the boot
	// loader samples it for us and we keep it running off the generic timer,
	// which counts from power on and never resets.
	sHardwareTime = args->platform_args.firmware_time;
	sHardwareTimeUptime = system_time();

	// Print both halves of the seed: a wall clock that ends up wrong is either
	// off by the whole seed or off by however much the counter has miscounted
	// since, and telling those two apart afterwards is otherwise guesswork.
	dprintf("arch_rtc_init: firmware time %" B_PRIu64 " s at %" B_PRIdBIGTIME
		" us uptime\n", sHardwareTime, sHardwareTimeUptime);

	return B_OK;
}


uint64
arch_rtc_get_hw_time(void)
{
	if (sHardwareTime == 0)
		return 0;

	return sHardwareTime + (system_time() - sHardwareTimeUptime) / 1000000;
}


void
arch_rtc_set_hw_time(uint64 seconds)
{
	// Nothing to write to: the firmware clock is out of reach, so a time set
	// at runtime does not survive a reboot.
}


void
arch_rtc_set_system_time_offset(struct real_time_data *data, bigtime_t offset)
{
	atomic_set64(&data->arch_data.system_time_offset, offset);
}


bigtime_t
arch_rtc_get_system_time_offset(struct real_time_data *data)
{
	return atomic_get64(&data->arch_data.system_time_offset);
}
