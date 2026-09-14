/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_DEVICE_WATCHDOG_H
#define _KERNEL_DEVICE_WATCHDOG_H


#include <SupportDefs.h>


/*!	A general kernel liveness watchdog for device drivers.

	This generalizes the pattern proven by the ENA network driver
	(src/add-ons/kernel/drivers/network/ether/ena/, documented in that
	directory's docs/watchdog-design.md): a driver registers a device, feeds a
	heartbeat from its healthy path, and supplies a recovery callback the
	framework invokes when the heartbeat goes stale for too long. On a headless
	cloud instance (Graviton EC2 has no console and no video device), a wedged
	device is often an unreachable machine, so a driver that resets its own
	device without being asked is the difference between a recoverable hiccup and
	a terminated instance.

	The two hard-won properties from the ENA design that this framework bakes in:

	- Observing is separated from deciding. device_watchdog_pet() only records a
	  timestamp and is safe to call from interrupt context; everything that can
	  block (the recovery callback) runs on the framework's own thread. A single
	  monitor thread runs at an elevated priority so that scheduler latency is
	  never misread as device death.
	- A single stale sample never triggers recovery. A configurable number of
	  consecutive missed deadlines is required, and a device whose recovery
	  callback fails is latched dead and left alone rather than reset in a loop --
	  a reset loop makes a console-less host strictly worse than the wedged device
	  it was trying to fix.
*/


// Opaque handle returned by register_device_watchdog().
typedef struct device_watchdog device_watchdog;


/*!	Attempts to recover a device whose heartbeat went stale. Called on the
	watchdog thread (never in interrupt context), so it may take locks, issue
	commands and block. Returns \c B_OK if the device is healthy again -- the
	framework then resumes watching it with a fresh heartbeat. Any other return
	latches the device dead: the framework logs it and stops watching, so a
	device that cannot be recovered is not reset endlessly.
*/
typedef status_t (*device_watchdog_recover_func)(void* cookie);

/*!	Optional liveness probe evaluated once per interval on the watchdog thread.
	Returning \c true is equivalent to a fresh heartbeat and clears the miss
	count -- direct evidence the device is making progress, used to grant a
	merely-late heartbeat extra patience (as ENA does with datapath traffic).
	May be \c NULL.
*/
typedef bool (*device_watchdog_probe_func)(void* cookie);


// Defaults used when the corresponding parameter is left zero/negative.
#define DEVICE_WATCHDOG_DEFAULT_INTERVAL	1000000		// 1 s
#define DEVICE_WATCHDOG_DEFAULT_MISSES		2


typedef struct device_watchdog_parameters {
	const char*						name;
		// A short identifier for logs and the "device_watchdogs" debugger
		// command. Not copied -- must outlive the registration.
	bigtime_t						timeout;
		// Heartbeat deadline in microseconds: silence longer than this counts as
		// one missed deadline. Required (> 0).
	bigtime_t						interval;
		// Check cadence in microseconds. <= 0 selects
		// DEVICE_WATCHDOG_DEFAULT_INTERVAL.
	int32							misses_before_recover;
		// Consecutive missed deadlines required before recover() is called.
		// <= 0 selects DEVICE_WATCHDOG_DEFAULT_MISSES. A value of 1 acts on a
		// single sample and is discouraged (see ENA's measured false-reset work).
	device_watchdog_recover_func	recover;
		// Required.
	device_watchdog_probe_func		probe;
		// Optional, may be NULL.
	void*							cookie;
		// Passed unmodified to recover() and probe().
} device_watchdog_parameters;


#ifdef __cplusplus
extern "C" {
#endif

status_t device_watchdog_init(void);
status_t device_watchdog_init_post_thread(void);

/*!	Registers a device for liveness monitoring. Monitoring starts enabled with
	the heartbeat seeded to "now", so the deadline is measured from registration
	rather than from the epoch. Returns \c NULL on invalid parameters or memory
	pressure. Call from normal thread context (driver attach), never from an
	interrupt.
*/
device_watchdog* register_device_watchdog(
			const device_watchdog_parameters* parameters);

/*!	Unregisters and frees a watchdog. Blocks until any recovery callback in
	flight for this device has finished, so on return no callback is running and
	the handle (and anything its cookie points at) is safe to tear down. Call
	from normal thread context (driver detach), never from an interrupt.
*/
void unregister_device_watchdog(device_watchdog* watchdog);

/*!	Records a heartbeat: the device is alive as of now. Lock-free and safe to
	call from any context including interrupt handlers -- this is the "observe"
	half that the datapath calls, kept cheap on purpose.
*/
void device_watchdog_pet(device_watchdog* watchdog);

/*!	Pauses or resumes monitoring without unregistering. Use to stop the watchdog
	while the interface is administratively down, or while the driver is itself
	driving a reset, so those windows are not mistaken for device death. Enabling
	seeds a fresh heartbeat and clears the accumulated miss count.
*/
void device_watchdog_set_enabled(device_watchdog* watchdog, bool enabled);

#ifdef __cplusplus
}
#endif

#endif	/* _KERNEL_DEVICE_WATCHDOG_H */
