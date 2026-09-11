/*
 * Copyright 2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Minimal read-only PMUv3 counter facility for arm64.

	This exists so that the arm64/Graviton performance work has something to
	measure with. Everything AWS's perfrunbook prescribes -- ipc,
	stall_*_pkc, *-mpki, data-tlb-tw-pki -- is a ratio of two PMU counters,
	and until now the port could read none of them.

	What this is: a fixed set of events programmed per CPU, read on demand,
	printed by a KDL command or by arm64_pmu_dump() from kernel code.

	What this is, additionally (E-PMU-1a/1b): one event counter is dedicated to
	sampling. It is programmed to CPU_CYCLES and preloaded so it overflows once
	every sample period; the overflow raises the PMU's PPI, whose handler
	reloads the counter (so the rate is bounded -- no storm) and bumps a
	per-CPU overflow count. PMCCNTR_EL0 is left free-running and untouched by
	sampling, because arm64_pmu_measure_core_frequency() and the per-thread
	cycle accounting both rely on it being monotonic; reloading it would inject
	a discontinuity every period.

	What this is, additionally (E-PMU-2): each overflow is a cycle-attributed
	profiling sample. The handler calls system_profiler_hardware_sample(), which
	walks the interrupted thread's stack -- the same path the software profiling
	timer uses on every architecture -- into the active profiler's buffer. So
	while a `profile`/system_profiler session runs on arm64, the samples come
	from the PMU cycle counter rather than a wall-clock timer, and the software
	timer stands down (arm64_pmu_sampling_active()) to keep the PMU the single
	source. When no session is running the call is inert.

	The timer stands down only once an overflow has actually been serviced, not
	merely once the handler is installed -- see arm64_pmu_sampling_active(). The
	overflow interrupt DOES fire on AWS Graviton (Neoverse-V1/c7g, hardware
	verified): with the facility on and a CPU-bound load, the per-CPU overflow
	count advances steadily and is bounded (no storm). The INTID is taken from
	the MADT GICC "Performance Interrupt GSIV" (parsed in the boot loader,
	carried in intc_info::pmu_gsiv, read in arm64_pmu_init()); on the Graviton
	parts measured it reports 23 -- the architected PPI 7 -- which is correct.
	Where the firmware states no GSIV, sampling stays off and profiling uses the
	software timer, as it does on every other architecture.

	One caveat remains, and it is a property of the virtualized platform, not of
	this code: on a guest the serviced overflow rate runs well below the rate the
	programmed period implies (~tens/s per core rather than the ~260/s that
	10^7 cycles at ~2.6 GHz would give). The reload value and the re-arm here are
	correct -- the event counter is preloaded to -period, PMOVSCLR is cleared and
	the counter reloaded on every serviced overflow, and PMCNTENSET/PMINTENSET
	stay set -- but the guest only counts CPU_CYCLES while its vCPU is actually
	scheduled, and the overflow interrupt is delivered when the hypervisor injects
	it rather than at the instant the counter wraps, so a tight guest loop that
	rarely exits sees far fewer interrupts than it has overflows. The per-core
	variance in the observed count is the signature of this: delivery tracks each
	core's exit/schedule rate, not the period. Direct counter reads (the core
	clock measurement below) are unaffected because they need no injection. The
	fix for the low rate, if one is possible, is not in this file; it needs
	hardware confirmation on a bare-metal instance, where the reload code should
	yield the full ~260/s with no hypervisor in the delivery path.

	What this deliberately is not:

	- Not a userland interface of its own. There is no syscall, no /dev node, no
	  per-thread accounting, and PMUSERENR_EL0 is explicitly written to zero
	  rather than opening the counters to EL0. Three reasons: the counters are
	  not saved or restored across a context switch, so an EL0 reader would
	  see whatever else ran on that core; PMCCNTR_EL0 at EL0 is a
	  high-resolution timing side channel; and nothing in userland could
	  consume the values yet anyway. Zeroing it is also a small hardening win,
	  since the register's reset value is not architecturally guaranteed.
	- Not per-thread or system-wide. A counter belongs to one core and is
	  never migrated or summed, which is why every reading carries a CPU
	  number.

	Enabling is opt-in, via the "arm64_pmu" boot setting or the "pmu on" KDL
	command, and not because the facility is dangerous in itself: on a
	virtualized Graviton instance the hypervisor at EL2 may trap EL1 accesses
	to the PMU registers (MDCR_EL2.TPM), which would fault in a kernel that
	has no EL2 handler left. AWS documents full PMU support only for the
	16xlarge and metal sizes. The same image has to boot on the other sizes,
	so the default is to detect and report, and touch nothing.
*/


#include <arch/arm64/arch_pmu.h>

#include <KernelExport.h>
#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <interrupt_controller.h>
#include <debug.h>
#include <interrupts.h>
#include <safemode.h>
#include <smp.h>
#include <system_profiler.h>

#include <stdio.h>
#include <string.h>


// The boot setting that turns the facility on for every CPU from early boot.
#define ARM64_PMU_SAFEMODE_OPTION	"arm64_pmu"


// The PMUv3 counter-overflow interrupt number is NOT derivable from any PMU
// register. The device-tree binding for "arm,armv8-pmuv3" recommends
// <GIC_PPI 7 ...> (GIC-500 TRM Table A.3), which maps to GIC INTID 23 (PPI +
// 16); the architected timer's PPI is handled the same way (arch_timer.cpp
// hardcodes INTID 27). But that PPI is only a recommendation, and firmware is
// free to place the PMU interrupt on a different line -- the authoritative
// value on an ACPI system is the MADT GICC "Performance Interrupt GSIV". So the
// INTID is taken from the MADT (parsed in the boot loader, carried in
// intc_info::pmu_gsiv) rather than assumed here; see sOverflowIntID. On the
// Graviton parts measured the firmware reports 23 -- the architected value --
// and the overflow interrupt fires correctly on it, so keeping the number
// authoritative rather than hardcoded costs nothing and stays correct on any
// part whose firmware places it elsewhere. When the firmware states no GSIV,
// PMU sampling stays off and the software profiling timer runs as on every
// other architecture.
#define PMU_OVERFLOW_ARCHITECTED_INTID	23

// Cycles between sampling-counter overflows. At ~2.6 GHz, 10^7 cycles is about
// 260 overflows per second per CPU under a fully CPU-bound load -- plenty of
// resolution for a profiler, and orders of magnitude below any rate that could
// storm. This is the default until a profiling session requests a specific
// interval via arm64_pmu_set_sample_interval().
#define PMU_DEFAULT_SAMPLE_PERIOD	10000000ULL

// Floor on the sampling period, in cycles. A `profile -i` request is converted
// to a cycle count against the measured core clock and clamped up to this, so a
// tiny interval cannot turn the overflow PPI into an interrupt storm. At the
// ~2.6-3.3 GHz Graviton parts measured here this caps delivery near ~10-13k
// overflows per second per CPU -- much finer than the 10^7-cycle default, and
// still far below any rate that could storm a core.
#define PMU_MIN_SAMPLE_PERIOD		250000ULL


// How each event's ratio is conventionally expressed, following the names
// AWS's perfrunbook uses.
enum pmu_ratio_kind {
	PMU_RATIO_NONE,
		// A denominator, or a raw count with no useful ratio.
	PMU_RATIO_MPKI,
		// Misses per kilo instruction.
	PMU_RATIO_PKI,
		// Events (not misses) per kilo instruction.
	PMU_RATIO_PKC,
		// Per kilo cycle, which is how stalls are expressed.
};


struct pmu_event_info {
	uint16			event;
	const char*		name;
	pmu_ratio_kind	ratio;
};


// Events we know how to name, and how each one's ratio is conventionally
// expressed. Programming an event that is not in here is allowed
// ("pmu events <n>"); it just prints as a bare count with no ratio.
static const pmu_event_info kEventNames[] = {
	{ PMU_EVENT_SW_INCR,			"sw-incr",			PMU_RATIO_NONE },
	{ PMU_EVENT_L1I_CACHE_REFILL,	"inst-l1",			PMU_RATIO_MPKI },
	{ PMU_EVENT_L1I_TLB_REFILL,		"inst-tlb",			PMU_RATIO_MPKI },
	{ PMU_EVENT_L1D_CACHE_REFILL,	"data-l1",			PMU_RATIO_MPKI },
	{ PMU_EVENT_L1D_CACHE,			"data-l1-access",	PMU_RATIO_PKI },
	{ PMU_EVENT_L1D_TLB_REFILL,		"data-tlb",			PMU_RATIO_MPKI },
	{ PMU_EVENT_INST_RETIRED,		"inst-retired",		PMU_RATIO_NONE },
	{ PMU_EVENT_EXC_TAKEN,			"exc-taken",		PMU_RATIO_PKI },
	{ PMU_EVENT_BR_MIS_PRED,		"branch-spec-miss",	PMU_RATIO_MPKI },
	{ PMU_EVENT_CPU_CYCLES,			"cpu-cycles",		PMU_RATIO_NONE },
	{ PMU_EVENT_BR_PRED,			"branch-spec",		PMU_RATIO_PKI },
	{ PMU_EVENT_MEM_ACCESS,			"mem-access",		PMU_RATIO_PKI },
	{ PMU_EVENT_L1I_CACHE,			"inst-l1-access",	PMU_RATIO_PKI },
	{ PMU_EVENT_L2D_CACHE,			"l2-access",		PMU_RATIO_PKI },
	{ PMU_EVENT_L2D_CACHE_REFILL,	"l2",				PMU_RATIO_MPKI },
	{ PMU_EVENT_BUS_ACCESS,			"bus-access",		PMU_RATIO_PKI },
	{ PMU_EVENT_INST_SPEC,			"inst-spec",		PMU_RATIO_NONE },
	{ PMU_EVENT_BR_RETIRED,			"branch",			PMU_RATIO_PKI },
	{ PMU_EVENT_BR_MIS_PRED_RETIRED, "branch",			PMU_RATIO_MPKI },
	{ PMU_EVENT_STALL_FRONTEND,		"stall_frontend",	PMU_RATIO_PKC },
	{ PMU_EVENT_STALL_BACKEND,		"stall_backend",	PMU_RATIO_PKC },
	{ PMU_EVENT_L1D_TLB,			"data-tlb-access",	PMU_RATIO_PKI },
	{ PMU_EVENT_L1I_TLB,			"inst-tlb-access",	PMU_RATIO_PKI },
	{ PMU_EVENT_L2D_TLB_REFILL,		"l2-tlb",			PMU_RATIO_MPKI },
	{ PMU_EVENT_L2D_TLB,			"l2-tlb-access",	PMU_RATIO_PKI },
	{ PMU_EVENT_DTLB_WALK,			"data-tlb-tw",		PMU_RATIO_PKI },
	{ PMU_EVENT_ITLB_WALK,			"inst-tlb-tw",		PMU_RATIO_PKI },
	{ PMU_EVENT_LL_CACHE_RD,		"l3-access",		PMU_RATIO_PKI },
	{ PMU_EVENT_LL_CACHE_MISS_RD,	"l3",				PMU_RATIO_MPKI },
};


struct pmu_preset {
	const char*	name;
	const char*	description;
	uint16		events[ARM64_PMU_MAX_EVENT_COUNTERS];
	uint32		eventCount;
};


// Six events each, because that is how many programmable counters
// Neoverse-N1 has (PMCR_EL0.N == 6). If a core has fewer, the tail of the set
// is dropped rather than silently mis-programmed.
static const pmu_preset kPresets[] = {
	{
		"core", "ipc and the frontend/backend stall split",
		{
			PMU_EVENT_INST_RETIRED,
			PMU_EVENT_STALL_FRONTEND,
			PMU_EVENT_STALL_BACKEND,
			PMU_EVENT_L1D_CACHE_REFILL,
			PMU_EVENT_L2D_CACHE_REFILL,
			PMU_EVENT_L1D_TLB_REFILL,
		}, 6
	},
	{
		"cache", "instruction and data cache miss rates",
		{
			PMU_EVENT_INST_RETIRED,
			PMU_EVENT_L1D_CACHE_REFILL,
			PMU_EVENT_L1I_CACHE_REFILL,
			PMU_EVENT_L2D_CACHE_REFILL,
			PMU_EVENT_LL_CACHE_RD,
			PMU_EVENT_LL_CACHE_MISS_RD,
		}, 6
	},
	{
		"tlb", "translation misses and page table walks",
		{
			PMU_EVENT_INST_RETIRED,
			PMU_EVENT_L1D_TLB_REFILL,
			PMU_EVENT_L1I_TLB_REFILL,
			PMU_EVENT_L2D_TLB_REFILL,
			PMU_EVENT_DTLB_WALK,
			PMU_EVENT_ITLB_WALK,
		}, 6
	},
	{
		"branch", "branch prediction",
		{
			PMU_EVENT_INST_RETIRED,
			PMU_EVENT_BR_RETIRED,
			PMU_EVENT_BR_MIS_PRED_RETIRED,
			PMU_EVENT_BR_PRED,
			PMU_EVENT_BR_MIS_PRED,
			PMU_EVENT_STALL_FRONTEND,
		}, 6
	},
};


struct pmu_cpu_state {
	bool	programmed;
	uint32	counterCount;
	uint16	events[ARM64_PMU_MAX_EVENT_COUNTERS];
	uint32	last[ARM64_PMU_MAX_EVENT_COUNTERS];
	uint64	total[ARM64_PMU_MAX_EVENT_COUNTERS];
	bool	sampling;
		// The dedicated sampling counter is programmed and its overflow armed
		// on this CPU. Never set unless the facility is enabled.
	uint64	overflowCount;
		// Sampling-counter overflow interrupts serviced on this CPU since the
		// last reset. Bumped only by the overflow handler; read from KDL.
} CACHE_LINE_ALIGN;
	// Aligned so that one CPU's block never shares a cache line with another's.


static pmu_cpu_state sPerCPU[SMP_MAX_CPUS];

static bool sAvailable;
	// ID_AA64DFR0_EL1 says PMUv3 is implemented. Says nothing about whether
	// EL2 lets us reach it.
static bool sEnabled;
	// The facility has been asked for. Per-CPU programming follows.
static bool sLongEventCounters;
	// FEAT_PMUv3p5: the event counters are 64 bit, so they do not wrap.
static uint64 sPmuVer;
static uint32 sCounterCount;
	// PMCR_EL0.N, read the first time a CPU is programmed.
static uint64 sEventMap0;
static uint64 sEventMap1;
	// PMCEID0/1_EL0, likewise.
static bool sCounterInfoValid;

static uint32 sSampleCounter;
	// The event-counter index reserved for the CPU_CYCLES sampling source
	// (E-PMU-1a). Identical on every core, since all PEs in an instance are the
	// same part. Set once, in pmu_read_implementation_info().
static uint64 sSamplePeriod = PMU_DEFAULT_SAMPLE_PERIOD;
	// Cycles between sampling overflows.
static uint32 sOverflowIntID;
	// The GIC INTID to install the overflow handler on, taken from the MADT
	// GICC "Performance Interrupt GSIV" (via intc_info::pmu_gsiv) in
	// arm64_pmu_init(). Zero means the firmware did not state it, in which case
	// PMU sampling stays off and the software profiling timer runs instead --
	// no fault, just a graceful degrade. Never assume the architected INTID 23:
	// it does not match what Graviton's firmware reports.
static bool sOverflowInterruptInstalled;
	// The PMU overflow PPI handler has been installed and its GIC line enabled.
	// Idempotent guard; the install happens once, from normal context.
static bool sOverflowObserved;
	// Set the first time the overflow handler actually services an overflow, so
	// arm64_pmu_sampling_active() reports the PMU as a sample source only once it
	// has proven it delivers -- being installed is not the same as firing. Racy
	// write from interrupt context is fine: it is a monotonic false->true latch
	// and a stale read only costs one profiling window on the software timer.

static const pmu_preset* sPreset = &kPresets[0];
static uint16 sEvents[ARM64_PMU_MAX_EVENT_COUNTERS];
static uint32 sEventCount;


static const pmu_event_info*
event_info(uint16 event)
{
	for (size_t i = 0; i < B_COUNT_OF(kEventNames); i++) {
		if (kEventNames[i].event == event)
			return &kEventNames[i];
	}

	return NULL;
}


static const char*
event_name(uint16 event)
{
	const pmu_event_info* info = event_info(event);
	return info != NULL ? info->name : "?";
}


static const char*
ratio_suffix(pmu_ratio_kind ratio)
{
	switch (ratio) {
		case PMU_RATIO_MPKI:
			return "mpki";
		case PMU_RATIO_PKI:
			return "pki";
		case PMU_RATIO_PKC:
			return "pkc";
		default:
			return "";
	}
}


static bool
event_implemented(uint16 event)
{
	if (!sCounterInfoValid)
		return true;

	return PMCEID_EVENT_IMPLEMENTED(sEventMap0, sEventMap1, event) != 0;
}


static inline void
pmu_select_counter(uint32 counter)
{
	WRITE_SPECIALREG(PMSELR_EL0, counter & PMSELR_SEL_MASK);

	// PMXEVTYPER_EL0/PMXEVCNTR_EL0 resolve PMSELR_EL0 when they are accessed,
	// and the architecture does not order a write to one system register
	// against a later access to a different one. Without this barrier the
	// alias can still be pointing at the previously selected counter.
	arm64_isb();
}


/*!	The exception-level filter bits to OR into PMCCFILTR_EL0 and every
	PMEVTYPER_EL0, so a counter counts at the exception level the kernel actually
	runs at.

	With every filter bit clear a counter counts at EL1 and EL0 but NOT at EL2:
	unlike P (EL1) and U (EL0), which are inhibits, NSH is an *enable* for EL2
	(Non-secure Hyp). On a virtualized instance the kernel runs at EL1, so zero
	is correct. On bare metal it runs at EL2 as a VHE host (#224); there, with
	NSH clear, PMCCNTR_EL0 and every event counter freeze while executing in the
	kernel -- which zeroed arm64_pmu_measure_core_frequency() (sysinfo 0 MHz,
	profile -i a silent no-op, #240) and would have the sampling counter never
	overflow, delivering no profiling samples at all on metal. Set NSH exactly
	when the kernel is at EL2 so the counters count where the code runs. Reading
	CurrentEL never traps; the timer path picks its EL2 register set the same way
	(arch_timer.cpp).
*/
static uint64
pmu_exception_filter_bits()
{
	return (READ_SPECIALREG(CurrentEL) >> 2) >= 2 ? PMEVTYPER_NSH : 0;
}


static void
pmu_read_implementation_info()
{
	// Racy at boot, harmlessly: every core in an instance is identical, so
	// concurrent callers write the same values.
	if (sCounterInfoValid)
		return;

	uint64 pmcr = READ_SPECIALREG(PMCR_EL0);
	sCounterCount = PMCR_N(pmcr);
	if (sCounterCount > ARM64_PMU_MAX_EVENT_COUNTERS)
		sCounterCount = ARM64_PMU_MAX_EVENT_COUNTERS;

	// Reserve the top event counter as the sampling source (E-PMU-1a). The
	// general read facility gets the counters below it. On the 6-counter
	// Neoverse parts this leaves 5 for the presets, which therefore program
	// their first 5 events; the sixth is dropped, the same way the tail is
	// dropped on a core with fewer counters.
	sSampleCounter = sCounterCount > 0 ? sCounterCount - 1 : 0;

	sEventMap0 = READ_SPECIALREG(PMCEID0_EL0);
	sEventMap1 = READ_SPECIALREG(PMCEID1_EL0);

	sCounterInfoValid = true;
}


static void
pmu_select_events(const uint16* events, uint32 count)
{
	if (count > ARM64_PMU_MAX_EVENT_COUNTERS)
		count = ARM64_PMU_MAX_EVENT_COUNTERS;

	memcpy(sEvents, events, count * sizeof(uint16));
	sEventCount = count;
}


/*!	The value to preload into the sampling counter so it overflows after
	exactly one period. The counters count up, so this is the two's-complement
	of the period: a 32 bit counter (the default; Neoverse-N1/-V1) keeps only
	the low 32 bits and overflows at bit 31, a 64 bit counter (FEAT_PMUv3p5,
	PMCR_EL0.LP set) keeps all 64 and overflows at bit 63 -- writing the
	full-width negation is correct for both.
*/
static uint64
pmu_sample_reload_value()
{
	if (sLongEventCounters)
		return 0ULL - sSamplePeriod;

	return (uint64)(uint32)(0ULL - sSamplePeriod);
}


/*!	Programs and enables the counters of the CPU this is executing on. \a cpu
	only says which per-CPU state block to keep the software counter extension
	in, so the caller has to guarantee it is the CPU actually running this;
	interrupts must be disabled for the same reason.
*/
static void
pmu_program_cpu(int32 cpu)
{
	pmu_cpu_state& state = sPerCPU[cpu];

	pmu_read_implementation_info();

	// Stop everything first, and in particular make sure no overflow
	// interrupt can be raised: we never want one, and neither firmware nor a
	// prior EL2 is required to have left PMINTEN clear.
	WRITE_SPECIALREG(PMCR_EL0, 0);
	WRITE_SPECIALREG(PMINTENCLR_EL1, PMU_ALL_COUNTERS_MASK);
	WRITE_SPECIALREG(PMCNTENCLR_EL0, PMU_ALL_COUNTERS_MASK);
	WRITE_SPECIALREG(PMOVSCLR_EL0, PMU_ALL_COUNTERS_MASK);

	// Deny EL0 every form of access. See the file comment for why.
	WRITE_SPECIALREG(PMUSERENR_EL0, 0);

	// The exception levels a counter is allowed to count at. P and U are clear
	// (count at EL1 and EL0); NSH is set only when the kernel is at EL2, so a
	// VHE host on bare metal counts cycles/events while in the kernel rather
	// than freezing every counter there. See pmu_exception_filter_bits().
	uint64 filter = pmu_exception_filter_bits();

	// Count cycles at EL1/EL0, and at EL2 when that is where the kernel runs.
	WRITE_SPECIALREG(PMCCFILTR_EL0, filter);

	// One event counter is dedicated to sampling (E-PMU-1a); the general read
	// facility uses the counters below it. Reaching pmu_program_cpu() at all
	// means the facility is enabled, so sampling is always set up here.
	bool sampling = sCounterCount > 0;
	uint32 generalCounters = sampling ? sCounterCount - 1 : sCounterCount;

	uint32 count = sEventCount;
	if (count > generalCounters)
		count = generalCounters;

	uint32 enableMask = PMU_CYCLE_COUNTER_BIT;
	for (uint32 i = 0; i < count; i++) {
		state.events[i] = sEvents[i];
		state.last[i] = 0;
		state.total[i] = 0;

		pmu_select_counter(i);
		WRITE_SPECIALREG(PMXEVTYPER_EL0,
			((uint64)sEvents[i] & PMEVTYPER_EVTCOUNT_MASK) | filter);
		WRITE_SPECIALREG(PMXEVCNTR_EL0, 0);

		enableMask |= PMU_EVENT_COUNTER_BIT(i);
	}

	state.counterCount = count;
	state.sampling = sampling;
	state.overflowCount = 0;

	// The sampling counter counts CPU_CYCLES. Select and type it here, but do
	// not preload its value yet: PMCR_P below resets every event counter to
	// zero, so the -period value has to be written *after* the PMCR write.
	if (sampling) {
		pmu_select_counter(sSampleCounter);
		WRITE_SPECIALREG(PMXEVTYPER_EL0,
			((uint64)PMU_EVENT_CPU_CYCLES & PMEVTYPER_EVTCOUNT_MASK) | filter);
		enableMask |= PMU_EVENT_COUNTER_BIT(sSampleCounter);
	}

	WRITE_SPECIALREG(PMCNTENSET_EL0, enableMask);

	// E enables the counters, P and C zero the event counters and the cycle
	// counter respectively, LC makes the cycle counter overflow at bit 63
	// instead of bit 31, and D is left clear so the cycle counter counts every
	// cycle rather than every 64th. LP is the equivalent of LC for the event
	// counters and only exists from FEAT_PMUv3p5, so it is conditional: on
	// Neoverse-N1 and -V1 the event counters really are 32 bit.
	uint64 pmcr = PMCR_E | PMCR_P | PMCR_C | PMCR_LC;
	if (sLongEventCounters)
		pmcr |= PMCR_LP;
	WRITE_SPECIALREG(PMCR_EL0, pmcr);
	arm64_isb();

	// Now that PMCR_P's reset is behind us, preload the sampling counter and
	// arm *only* its overflow interrupt (PMINTENCLR above masked every
	// counter, so a wrap of any general counter never raises an IRQ). Arming
	// PMINTENSET last, after the counter already holds the bounded reload
	// value, guarantees the first overflow is one full period away and can
	// never fire against a freshly-zeroed counter. Delivery to the CPU still
	// requires the GIC PPI line, enabled once by pmu_install_overflow_interrupt();
	// until then an overflow only latches PMOVSCLR (no storm), and the handler
	// consumes it as soon as the line comes up.
	if (sampling) {
		pmu_select_counter(sSampleCounter);
		WRITE_SPECIALREG(PMXEVCNTR_EL0, pmu_sample_reload_value());
		WRITE_SPECIALREG(PMOVSCLR_EL0, PMU_EVENT_COUNTER_BIT(sSampleCounter));
		WRITE_SPECIALREG(PMINTENSET_EL1, PMU_EVENT_COUNTER_BIT(sSampleCounter));
		arm64_isb();
	}

	state.programmed = true;
}


static void
pmu_stop_cpu(int32 cpu)
{
	WRITE_SPECIALREG(PMCR_EL0, 0);
	// Mask the overflow interrupt as well, so a "pmu off" on this CPU leaves
	// the sampling counter fully disarmed: no PMINTEN, no enable, flags clear.
	WRITE_SPECIALREG(PMINTENCLR_EL1, PMU_ALL_COUNTERS_MASK);
	WRITE_SPECIALREG(PMCNTENCLR_EL0, PMU_ALL_COUNTERS_MASK);
	WRITE_SPECIALREG(PMOVSCLR_EL0, PMU_ALL_COUNTERS_MASK);
	arm64_isb();

	sPerCPU[cpu].programmed = false;
	sPerCPU[cpu].sampling = false;
}


/*!	Reloads the calling CPU's sampling counter to the current sSamplePeriod, so a
	period change takes effect on this core. Only touches the dedicated sampling
	counter; the general event counters and every enable/interrupt gate are left
	exactly as pmu_program_cpu() set them.

	Called on every CPU via call_all_cpus_sync() from arm64_pmu_set_sample_interval().
	That path runs this with interrupts disabled on each target CPU (an IPI on the
	remote cores, local processing with interrupts off on the calling core), so it
	is mutually exclusive with this same CPU's overflow handler -- the two never
	write the per-CPU sampling counter concurrently. That is the whole reason the
	reprogram is safe despite the handler otherwise owning the counter reload: the
	counter is per-CPU and never touched cross-core, and on its own CPU the reload
	and this reprogram cannot interleave.
*/
static void
pmu_reprogram_sample_counter(void* /*cookie*/, int cpu)
{
	pmu_cpu_state& state = sPerCPU[cpu];
	if (!state.sampling)
		return;

	// pmu_sample_reload_value() reads sSamplePeriod, which the caller published
	// before this fan-out; on a core whose overflow handler happens to reload
	// between that store and this write, both see the new period, so the counter
	// still ends up holding a full new period's worth of headroom.
	pmu_select_counter(sSampleCounter);
	WRITE_SPECIALREG(PMXEVCNTR_EL0, pmu_sample_reload_value());
	WRITE_SPECIALREG(PMOVSCLR_EL0, PMU_EVENT_COUNTER_BIT(sSampleCounter));
	arm64_isb();
}


//	#pragma mark - sampling overflow interrupt


/*!	PMU counter-overflow PPI handler. Runs in interrupt context on whichever
	CPU overflowed; the sampling counter and PMOVSCLR are per-CPU system
	registers, so it touches only this CPU's state.

	The reload is what bounds the interrupt rate. The overflow line is
	level-sensitive: were the counter left wrapped, it would keep the line
	asserted and re-fire the moment we returned -- an interrupt storm. Reloading
	it to -period makes the next overflow exactly one period away, so the rate
	is one interrupt per sSamplePeriod cycles and no more.

	Each overflow is a cycle-attributed sample: E-PMU-2 hands the interrupted
	thread to system_profiler_hardware_sample(), which walks the interrupted
	stack (the same path the software profiling timer uses) into the active
	profiler's buffer, so `profile`/`system_profiler` in userland attribute the
	samples to the code that was running. That call is inert whenever no
	sampling session is in progress, so the only cost then is bumping the
	per-CPU overflow count.
*/
static int32
pmu_overflow_interrupt(void* data)
{
	// Inert unless the facility is enabled. Anything reached below touches PMU
	// registers, which is only safe once the facility has been turned on (see
	// the file comment); if it is off, this is not our interrupt to handle.
	if (!sAvailable || !sEnabled)
		return B_UNHANDLED_INTERRUPT;

	int32 cpu = smp_get_current_cpu();
	pmu_cpu_state& state = sPerCPU[cpu];
	if (!state.sampling)
		return B_UNHANDLED_INTERRUPT;

	uint32 sampleBit = PMU_EVENT_COUNTER_BIT(sSampleCounter);
	uint32 overflow = (uint32)READ_SPECIALREG(PMOVSCLR_EL0);
	if ((overflow & sampleBit) == 0)
		return B_UNHANDLED_INTERRUPT;

	// Clear the sticky overflow flag (deasserts the level line) and reload the
	// counter before returning, so exactly one more period elapses before the
	// next interrupt.
	WRITE_SPECIALREG(PMOVSCLR_EL0, sampleBit);
	pmu_select_counter(sSampleCounter);
	WRITE_SPECIALREG(PMXEVCNTR_EL0, pmu_sample_reload_value());
	arm64_isb();

	state.overflowCount++;
	sOverflowObserved = true;

	// Hand the interrupted thread to the system profiler as a cycle-attributed
	// sample (E-PMU-2). A no-op unless a sampling session is active, so this is
	// free on an idle profiler and the overflow count above is the only work
	// done in the common case.
	system_profiler_hardware_sample();

	return B_HANDLED_INTERRUPT;
}


/*!	Installs the overflow PPI handler and enables its GIC line, once. Must run
	in normal thread context (it allocates and, for a PPI, fans the enable out
	to every CPU via call_all_cpus_sync), so it is never called from KDL.

	The per-CPU PMU-side gate (PMINTENSET_EL1) is armed separately, in
	pmu_program_cpu(); this only completes the delivery path for CPUs that have
	already armed it. A CPU that never programmed the PMU keeps its overflow
	masked and delivers nothing even though the shared GIC line is enabled.
*/
static void
pmu_install_overflow_interrupt()
{
	if (sOverflowInterruptInstalled || !sAvailable || !sEnabled)
		return;

	// The INTID comes from the MADT GICC Performance Interrupt GSIV, not from a
	// hardcoded PPI. Zero means the firmware did not state it: leave sampling
	// off (the software profiling timer keeps running) rather than guess.
	if (sOverflowIntID == 0) {
		dprintf("arm64_pmu: no PMU overflow interrupt GSIV from firmware; "
			"sampling disabled, software profiling timer used instead\n");
		return;
	}

	status_t status = install_io_interrupt_handler(sOverflowIntID,
		&pmu_overflow_interrupt, NULL, 0);
	if (status != B_OK) {
		dprintf("arm64_pmu: could not install overflow handler on INTID %"
			B_PRIu32 ": %s\n", sOverflowIntID, strerror(status));
		return;
	}

	sOverflowInterruptInstalled = true;
	dprintf("arm64_pmu: sampling overflow interrupt on INTID %" B_PRIu32
		" (madt gicc performance gsiv), counter [%" B_PRIu32 "] cpu-cycles, "
		"period %" B_PRIu64 " cycles\n", sOverflowIntID, sSampleCounter,
		sSamplePeriod);
}


//	#pragma mark - ratio formatting


/*!	Prints \a numerator / \a denominator with two decimal places, without
	touching the FPU (the kernel does not save VFP state around this).
*/
static void
print_ratio(void (*print)(const char*, ...), const char* label,
	uint64 numerator, uint64 denominator)
{
	if (denominator == 0) {
		print("  %-22s      n/a\n", label);
		return;
	}

	uint64 scaled = (numerator * 100) / denominator;
	print("  %-22s %6" B_PRIu64 ".%02" B_PRIu64 "\n", label, scaled / 100,
		scaled % 100);
}


static void
pmu_print_sample(void (*print)(const char*, ...),
	const arm64_pmu_sample& sample)
{
	if (!sample.valid) {
		print("pmu: not programmed on CPU %" B_PRId32 "\n", sample.cpu);
		return;
	}

	// The CPU number is not decoration. Counters are per-core and this reading
	// describes one core only.
	print("pmu: CPU %" B_PRId32 ", %" B_PRIu64 " cycles since reset\n",
		sample.cpu, sample.cycles);

	// The dedicated sampling counter and how many times its overflow has fired
	// on this CPU. Under a CPU-bound load this count should climb at roughly
	// (core clock / period) per second; a runaway would be the storm signature.
	if (sample.sampling) {
		print("pmu: sampling counter [%" B_PRIu32 "] cpu-cycles, period %"
			B_PRIu64 " cycles, %" B_PRIu64 " overflow interrupt(s) since "
			"reset\n", sample.sampleCounter, sample.samplePeriod,
			sample.sampleOverflows);
	}

	// Find instructions retired, if it is in the set, so the *-mpki ratios can
	// be computed. AWS's runbook expresses almost everything per kilo
	// instruction.
	uint64 instructions = 0;
	for (uint32 i = 0; i < sample.eventCount; i++) {
		if (sample.events[i] == PMU_EVENT_INST_RETIRED)
			instructions = sample.values[i];
	}

	for (uint32 i = 0; i < sample.eventCount; i++) {
		const char* wrapNote = "";
		if ((sample.wrapped & PMU_EVENT_COUNTER_BIT(i)) != 0)
			wrapNote = "  (overflowed, low by a multiple of 2^32)";

		print("  [%" B_PRIu32 "] %-20s 0x%04x %20" B_PRIu64 "%s\n", i,
			event_name(sample.events[i]), (unsigned int)sample.events[i],
			sample.values[i], wrapNote);
	}

	print("pmu: derived ratios\n");

	if (instructions != 0)
		print_ratio(print, "ipc", instructions, sample.cycles);
	else
		print("  %-22s      n/a (inst-retired not programmed)\n", "ipc");

	for (uint32 i = 0; i < sample.eventCount; i++) {
		const pmu_event_info* info = event_info(sample.events[i]);
		if (info == NULL || info->ratio == PMU_RATIO_NONE)
			continue;

		char label[48];
		snprintf(label, sizeof(label), "%s-%s", info->name,
			ratio_suffix(info->ratio));

		// Stalls are expressed per kilo cycle because the question they answer
		// is what fraction of the core's cycles went nowhere; everything else
		// is per kilo instruction, so that it stays comparable across runs that
		// executed different amounts of work.
		uint64 denominator = info->ratio == PMU_RATIO_PKC
			? sample.cycles : instructions;

		print_ratio(print, label, sample.values[i] * 1000, denominator);
	}
}


//	#pragma mark - public interface


bool
arm64_pmu_available(void)
{
	return sAvailable;
}


bool
arm64_pmu_enabled(void)
{
	return sEnabled;
}


bool
arm64_pmu_sampling_active(void)
{
	// The PMU cycle-overflow is a usable sampling source only once its handler
	// has actually serviced an overflow (sOverflowObserved), not merely once the
	// handler is installed. The two differ in practice: an overflow only fires
	// once a CPU-bound load has burned a full period of cycles, so on an idle
	// instance the handler can be installed and armed for a while before the
	// first overflow arrives. Gating on a serviced overflow keeps this
	// self-correcting: where the interrupt never arrives (reduced-PMU sizes, no
	// GSIV, facility off) the facility never claims to be the sample source and
	// the software timer keeps profiling. The consumer side re-checks this on
	// every software-timer tick (SystemProfiler::_ProfilingEvent), so a session
	// that starts before its workload runs still hands over to the PMU the moment
	// the workload drives a real overflow, rather than being stuck on the timer.
	// sEnabled/sAvailable/sOverflowInterruptInstalled are implied once an
	// overflow has been serviced, but are kept so a later "pmu off" cannot claim
	// the source.
	return sAvailable && sEnabled && sOverflowInterruptInstalled
		&& sOverflowObserved;
}


bool
arm64_pmu_pmccntr_usable(void)
{
	// Do NOT gate a PMCCNTR_EL0 read on sEnabled alone: the "arm64_pmu" boot
	// flag is set very early and STAYS set on hardware where PMU access is
	// trapped to EL2 (e.g. AWS sizes without full PMU) and in the window before
	// pmu_program_cpu() has run for this CPU -- reading PMCCNTR_EL0 in either
	// case faults. sPerCPU[].programmed is set only AFTER a successful
	// pmu_program_cpu() (its register writes would themselves have trapped
	// otherwise), so it is the true "safe to read on this core" signal -- the
	// same guard arm64_pmu_read() uses. Callers on the context-switch path run
	// with interrupts off, so the current-CPU index is stable here.
	if (!sAvailable)
		return false;
	return sPerCPU[smp_get_current_cpu()].programmed;
}


void
arm64_pmu_disable(void)
{
	if (!sAvailable)
		return;

	cpu_status state = disable_interrupts();
	pmu_stop_cpu(smp_get_current_cpu());
	restore_interrupts(state);
}


void
arm64_pmu_reset(void)
{
	if (!sAvailable)
		return;

	cpu_status state = disable_interrupts();
	int32 cpu = smp_get_current_cpu();
	if (sPerCPU[cpu].programmed)
		pmu_program_cpu(cpu);
	restore_interrupts(state);
}


void
arm64_pmu_read(arm64_pmu_sample* sample)
{
	memset(sample, 0, sizeof(*sample));

	// Interrupts off both pins us to one CPU for the whole reading -- a sample
	// stitched together from two cores would be nonsense -- and keeps the
	// PMSELR/PMXEV* pairs from being interleaved by anything else.
	cpu_status state = disable_interrupts();

	int32 cpu = smp_get_current_cpu();
	sample->cpu = cpu;

	pmu_cpu_state& perCPU = sPerCPU[cpu];
	if (!sAvailable || !perCPU.programmed) {
		restore_interrupts(state);
		return;
	}

	sample->cycles = READ_SPECIALREG(PMCCNTR_EL0);

	// The overflow flags are sticky, so clear what we just read to make the
	// next sample's flags mean "wrapped during the last interval". The one
	// exception is the sampling counter's bit: that flag is owned by the
	// overflow handler, which reloads the counter off it, so clearing it here
	// would race the handler and drop a sample.
	uint32 sampleBit = perCPU.sampling
		? PMU_EVENT_COUNTER_BIT(sSampleCounter) : 0;
	uint32 overflow = (uint32)READ_SPECIALREG(PMOVSCLR_EL0);
	uint32 toClear = overflow & ~sampleBit;
	if (toClear != 0)
		WRITE_SPECIALREG(PMOVSCLR_EL0, toClear);

	sample->eventCount = perCPU.counterCount;
	for (uint32 i = 0; i < perCPU.counterCount; i++) {
		pmu_select_counter(i);
		uint64 raw = READ_SPECIALREG(PMXEVCNTR_EL0);

		if (sLongEventCounters) {
			// PMCR_EL0.LP is set, so the hardware counter is the full 64 bits
			// and was zeroed when this CPU was programmed. Nothing to extend.
			perCPU.total[i] = raw;
		} else {
			// 32 bit hardware counter. Unsigned wraparound makes this the
			// correct delta across one overflow; more than one overflow between
			// two samples silently loses 2^32 each time, which is what the
			// wrapped bitmap exists to warn about. At Graviton clock rates a
			// counter incrementing once per cycle wraps in under two seconds,
			// so a long measurement window has to be sampled, not just read at
			// the end.
			uint32 now = (uint32)raw;
			perCPU.total[i] += (uint64)(uint32)(now - perCPU.last[i]);
			perCPU.last[i] = now;
		}

		sample->events[i] = perCPU.events[i];
		sample->values[i] = perCPU.total[i];
	}

	// Neither the cycle counter nor the sampling counter is a general event
	// counter, so keep both out of the per-event wrapped bitmap.
	sample->wrapped = overflow & ~PMU_CYCLE_COUNTER_BIT & ~sampleBit;
	sample->valid = true;

	sample->sampling = perCPU.sampling;
	sample->sampleCounter = sSampleCounter;
	sample->samplePeriod = sSamplePeriod;
	sample->sampleOverflows = perCPU.overflowCount;

	restore_interrupts(state);
}


void
arm64_pmu_dump(void)
{
	arm64_pmu_sample sample;
	arm64_pmu_read(&sample);
	pmu_print_sample(&dprintf, sample);
}


status_t
arm64_pmu_measure_core_frequency(uint64* _frequency)
{
	// ARMv8 has no register that states the core clock. CNTFRQ_EL0 is the
	// generic timer's frequency and is not a substitute -- on the Graviton
	// parts measured here it reads 1.05 GHz and 1.000 GHz on cores actually
	// clocked at 2.6 GHz and 3.3 GHz. The only thing that counts core cycles
	// is PMCCNTR_EL0, so the clock has to be derived by counting cycles over
	// a known interval of a clock whose rate *is* stated.
	//
	// That makes this measurement gated on the PMU facility being on, and
	// deliberately so: see the file comment. EL2 may trap EL1 accesses to the
	// PMU registers on a virtualized instance, and a fault here has no
	// handler. Reporting that the frequency is unknown is a far better outcome
	// than an unbootable kernel, so an unprogrammed PMU is an error return and
	// never a plausible-looking number.
	if (!sAvailable || !sEnabled)
		return B_NOT_SUPPORTED;

	uint64 timerFrequency = READ_SPECIALREG(CNTFRQ_EL0);
	if (timerFrequency == 0)
		return B_NOT_SUPPORTED;

	// A 1 ms window is ~10^6 timer ticks and ~10^6 core cycles at any clock
	// rate worth reporting, so quantization contributes well under one part in
	// 10^5 -- far below the precision anyone reads out of a MHz figure.
	const uint64 windowTicks = timerFrequency / 1000;
	if (windowTicks == 0)
		return B_NOT_SUPPORTED;

	uint64 best = 0;

	// Take the largest of several windows rather than a mean or a median.
	// Every way this measurement can be disturbed biases it in the same
	// direction: if the hypervisor deschedules this vCPU mid-window, or the
	// core clock-gates, CNTVCT_EL0 keeps advancing while PMCCNTR_EL0 does not,
	// so the sample comes out low. Nothing makes it come out high. The
	// maximum is therefore the closest estimate of the real clock, and
	// averaging would only fold the outages in.
	for (int attempt = 0; attempt < 5; attempt++) {
		cpu_status state = disable_interrupts();

		// Both counters have to be sampled as close together as possible at
		// each end, and neither read may be hoisted out of the window.
		arm64_isb();
		uint64 startTicks = READ_SPECIALREG(CNTVCT_EL0);
		uint64 startCycles = READ_SPECIALREG(PMCCNTR_EL0);
		arm64_isb();

		uint64 endTicks;
		do {
			endTicks = READ_SPECIALREG(CNTVCT_EL0);
		} while (endTicks - startTicks < windowTicks);

		arm64_isb();
		uint64 endCycles = READ_SPECIALREG(PMCCNTR_EL0);
		arm64_isb();

		restore_interrupts(state);

		uint64 ticks = endTicks - startTicks;
		uint64 cycles = endCycles - startCycles;
		if (ticks == 0 || cycles == 0)
			continue;

		// cycles / (ticks / timerFrequency), ordered so that the numerator
		// cannot overflow: cycles is at most a few million here.
		uint64 frequency = (cycles * timerFrequency) / ticks;
		if (frequency > best)
			best = frequency;
	}

	// A number this far from anything a real core runs at means the
	// measurement did not work, not that the core is unusual, and passing it
	// on would put a fabricated clock rate in front of the user. 50 MHz is
	// below any core this kernel can boot on and 20 GHz is above anything
	// silicon does, so the bound only ever catches a broken measurement.
	if (best < 50000000ULL || best > 20000000000ULL)
		return B_ERROR;

	*_frequency = best;
	return B_OK;
}


/*!	Sets the sampling period from a wall-clock \a interval in microseconds (the
	unit the profiler and `profile -i` use) and reprograms every CPU's sampling
	counter to it, so a profiling session's requested rate is actually honored.
	Before this, sSamplePeriod was only ever its 10^7-cycle initializer, so
	`profile -i` was silently ignored and the profiler's "expected ticks" (derived
	from the requested interval) could never match the delivered rate -- the
	accounting mismatch #103 mistook for a virtualization shortfall.

	The counter counts core cycles, so the interval is converted through the
	measured core clock (there is no register that states it; see
	arm64_pmu_measure_core_frequency()), and clamped up to PMU_MIN_SAMPLE_PERIOD
	so a tiny interval cannot storm the overflow PPI. Returns B_NOT_SUPPORTED when
	the facility is off (the caller then falls back to the software timer, which
	uses the interval directly, as on every other architecture), so a caller may
	invoke it unconditionally and ignore the result.

	Runs in normal thread context only -- it both measures the clock (interrupts
	off, ~ms) and fans the reprogram out with call_all_cpus_sync().
*/
status_t
arm64_pmu_set_sample_interval(bigtime_t interval)
{
	if (!sAvailable || !sEnabled)
		return B_NOT_SUPPORTED;
	if (interval <= 0)
		return B_BAD_VALUE;

	uint64 frequency;
	status_t status = arm64_pmu_measure_core_frequency(&frequency);
	if (status != B_OK)
		return status;

	// cycles = interval_us * freq_hz / 10^6. The profiler floors the interval at
	// B_DEBUG_MIN_PROFILE_INTERVAL (10 us) and freq is a GHz-scale count, so the
	// product stays far inside 64 bits for any interval a profiler would request.
	uint64 period = ((uint64)interval * frequency) / 1000000ULL;
	if (period < PMU_MIN_SAMPLE_PERIOD)
		period = PMU_MIN_SAMPLE_PERIOD;

	// Publish the new period before the fan-out. sSamplePeriod is a single aligned
	// 64-bit word, so an overflow handler reading it concurrently on any core sees
	// either the old or the new value -- never a torn one -- and either reload
	// value is self-consistent. The reprogram callback then runs with interrupts
	// disabled on every CPU (see pmu_reprogram_sample_counter), which serializes
	// it against that CPU's own overflow-driven reload.
	sSamplePeriod = period;
	call_all_cpus_sync(&pmu_reprogram_sample_counter, NULL);

	return B_OK;
}


//	#pragma mark - KDL command


static int
debug_pmu(int argc, char** argv)
{
	if (!sAvailable) {
		kprintf("PMUv3 is not implemented on this CPU "
			"(ID_AA64DFR0_EL1.PMUVer)\n");
		return 0;
	}

	// Everything below acts on the CPU the debugger happens to be running on.
	// There is no way to reach another core's counters from here -- they are
	// only accessible to that core, and an inter-processor call is not
	// something to do from KDL -- so use the arm64_pmu boot setting to have
	// every CPU counting from the start.
	int32 cpu = smp_get_current_cpu();
	const char* command = argc > 1 ? argv[1] : "dump";

	if (strcmp(command, "dump") == 0) {
		arm64_pmu_sample sample;
		arm64_pmu_read(&sample);
		pmu_print_sample(&kprintf, sample);
		return 0;
	}

	if (strcmp(command, "on") == 0 || strcmp(command, "reset") == 0) {
		sEnabled = true;
		pmu_program_cpu(cpu);

		const pmu_cpu_state& state = sPerCPU[cpu];
		kprintf("pmu: counting on CPU %" B_PRId32 ", %" B_PRIu32 " of %"
			B_PRIu32 " event counters in use\n", cpu, state.counterCount,
			sCounterCount);

		for (uint32 i = 0; i < state.counterCount; i++) {
			// An event the core does not implement counts nothing, which is
			// indistinguishable from an event that did not happen. PMCEID0/1
			// is the only way to tell the two apart, so say so up front.
			kprintf("  [%" B_PRIu32 "] 0x%04x %-20s%s\n", i,
				(unsigned int)state.events[i], event_name(state.events[i]),
				event_implemented(state.events[i])
					? "" : "  NOT IMPLEMENTED, will read zero");
		}

		if (state.sampling) {
			kprintf("  [%" B_PRIu32 "] 0x%04x cpu-cycles (sampling, period %"
				B_PRIu64 " cycles)\n", sSampleCounter,
				(unsigned int)PMU_EVENT_CPU_CYCLES, sSamplePeriod);

			// Enabling from KDL cannot install the GIC handler (it allocates
			// and does a cross-CPU call), so overflows only get counted once
			// the line is up. Use the arm64_pmu boot setting for the full path.
			if (!sOverflowInterruptInstalled) {
				if (sOverflowIntID == 0) {
					kprintf("pmu: no overflow interrupt gsiv in the madt; "
						"sampling is off and the overflow count stays 0\n");
				} else {
					kprintf("pmu: overflow interrupt (INTID %" B_PRIu32 ") not "
						"installed yet; overflow count stays 0 until the boot "
						"setting installs it\n", sOverflowIntID);
				}
			}
		}

		return 0;
	}

	if (strcmp(command, "off") == 0) {
		pmu_stop_cpu(cpu);
		kprintf("pmu: stopped on CPU %" B_PRId32 "\n", cpu);
		return 0;
	}

	if (strcmp(command, "set") == 0) {
		if (argc < 3) {
			kprintf("presets:\n");
			for (size_t i = 0; i < B_COUNT_OF(kPresets); i++) {
				kprintf("  %-8s %s%s\n", kPresets[i].name,
					kPresets[i].description,
					&kPresets[i] == sPreset ? "  (current)" : "");
			}
			return 0;
		}

		for (size_t i = 0; i < B_COUNT_OF(kPresets); i++) {
			if (strcmp(argv[2], kPresets[i].name) != 0)
				continue;

			sPreset = &kPresets[i];
			pmu_select_events(kPresets[i].events, kPresets[i].eventCount);
			pmu_program_cpu(cpu);
			kprintf("pmu: preset \"%s\" programmed on CPU %" B_PRId32
				" only\n", kPresets[i].name, cpu);
			return 0;
		}

		kprintf("no such preset: %s\n", argv[2]);
		return 0;
	}

	if (strcmp(command, "events") == 0) {
		uint16 events[ARM64_PMU_MAX_EVENT_COUNTERS];
		uint32 count = 0;

		for (int i = 2; i < argc && count < ARM64_PMU_MAX_EVENT_COUNTERS; i++)
			events[count++] = (uint16)parse_expression(argv[i]);

		if (count == 0) {
			kprintf("usage: pmu events <event number> ...\n");
			return 0;
		}

		sPreset = NULL;
		pmu_select_events(events, count);
		pmu_program_cpu(cpu);
		kprintf("pmu: %" B_PRIu32 " events programmed on CPU %" B_PRId32
			" only\n", count, cpu);
		return 0;
	}

	print_debugger_command_usage(argv[0]);
	return 0;
}


//	#pragma mark - initialization


status_t
arm64_pmu_init(kernel_args* args)
{
	// Only ID_AA64DFR0_EL1 is read here, which is an ordinary ID register and
	// not part of the PMU. No PMU register is touched until somebody asks,
	// because on a virtualized instance EL2 may trap those accesses.
	uint64 dfr0 = READ_SPECIALREG(ID_AA64DFR0_EL1);
	sPmuVer = ID_AA64DFR0_PMU_VER(dfr0);

	if (sPmuVer == ID_AA64DFR0_PMU_VER_NONE
		|| sPmuVer == ID_AA64DFR0_PMU_VER_IMPL) {
		// 0xf means an IMPLEMENTATION DEFINED monitor that is not PMUv3 and
		// that we have no business poking at.
		dprintf("arm64_pmu: no PMUv3 (ID_AA64DFR0_EL1.PMUVer %" B_PRIu64
			")\n", sPmuVer >> ID_AA64DFR0_PMU_VER_SHIFT);
		return B_OK;
	}

	sAvailable = true;
	sLongEventCounters = sPmuVer >= ID_AA64DFR0_PMU_VER_3_5;

	// The overflow interrupt is installed on the MADT GICC Performance
	// Interrupt GSIV, parsed by the boot loader and carried here in
	// intc_info::pmu_gsiv. Read it once, now, while kernel_args is still around.
	// Zero means the firmware did not state it, and PMU sampling stays off.
	sOverflowIntID = args->arch_args.interrupt_controller.pmu_gsiv;
	if (sOverflowIntID != 0) {
		dprintf("arm64_pmu: overflow interrupt gsiv from madt = %" B_PRIu32
			"%s\n", sOverflowIntID,
			sOverflowIntID == PMU_OVERFLOW_ARCHITECTED_INTID
				? " (architected PPI 7)" : " (differs from architected INTID "
					"23)");
	} else {
		dprintf("arm64_pmu: no overflow interrupt gsiv in madt; pmu sampling "
			"will stay off (software profiling timer used)\n");
	}

	pmu_select_events(kPresets[0].events, kPresets[0].eventCount);

	sEnabled = get_safemode_boolean_early(args, ARM64_PMU_SAFEMODE_OPTION,
		false);

	dprintf("arm64_pmu: PMUv3 present (PMUVer %" B_PRIu64 "), %s event "
		"counters, %s\n", sPmuVer >> ID_AA64DFR0_PMU_VER_SHIFT,
		sLongEventCounters ? "64 bit" : "32 bit",
		sEnabled ? "enabled by boot setting" : "disabled (use the \"pmu\" "
			"KDL command or the arm64_pmu boot setting)");

	return B_OK;
}


void
arm64_pmu_init_percpu(kernel_args* args, int cpu)
{
	if (!sAvailable || !sEnabled)
		return;

	// Interrupts are still off this early, but be explicit: this must program
	// the CPU it is running on and no other. The caller's index is used rather
	// than smp_get_current_cpu() because it already knows which CPU this is.
	cpu_status state = disable_interrupts();
	pmu_program_cpu(cpu);
	restore_interrupts(state);

	if (cpu != 0)
		return;

	// Report what was actually programmed once, from the boot CPU. The target
	// is a headless instance whose only output is the serial console, so this
	// has to be in the boot log rather than only reachable from KDL. All cores
	// in an instance are identical, so one report covers them.
	dprintf("arm64_pmu: %" B_PRIu32 " of %" B_PRIu32 " event counters "
		"programmed on all CPUs (1 reserved for sampling)\n",
		sPerCPU[cpu].counterCount, sCounterCount);

	for (uint32 i = 0; i < sPerCPU[cpu].counterCount; i++) {
		uint16 event = sPerCPU[cpu].events[i];

		// An event this core does not implement counts nothing, which reads
		// exactly like an event that never happened. PMCEID0/1_EL0 is the only
		// way to tell those apart, so it gets said here rather than left for
		// somebody to misread a zero later.
		dprintf("arm64_pmu:   [%" B_PRIu32 "] 0x%04x %s%s\n", i,
			(unsigned int)event, event_name(event),
			event_implemented(event)
				? "" : " -- NOT IMPLEMENTED, will read zero");
	}

	if (sPerCPU[cpu].sampling) {
		dprintf("arm64_pmu:   [%" B_PRIu32 "] 0x%04x cpu-cycles -- sampling, "
			"period %" B_PRIu64 " cycles\n", sSampleCounter,
			(unsigned int)PMU_EVENT_CPU_CYCLES, sSamplePeriod);
	}
}


status_t
arm64_pmu_init_post_modules(kernel_args* args)
{
	if (!sAvailable)
		return B_OK;

	add_debugger_command_etc("pmu", &debug_pmu,
		"Read the arm64 performance monitors on the current CPU",
		"[ dump | on | off | reset | set [<preset>] | events <n> ... ]\n"
		"Reads or programs the PMUv3 counters of the CPU the debugger is\n"
		"running on. Counters are per core: there is no way to read another\n"
		"CPU's counters from here, so use the \"arm64_pmu\" boot setting to\n"
		"have every CPU counting from boot.\n"
		"  dump    print the counters and the derived ratios (default)\n"
		"  on      program and start the counters\n"
		"  off     stop the counters\n"
		"  reset   zero the counters and start a new measurement window\n"
		"  set     list the event presets, or select one\n"
		"  events  program an explicit list of event numbers\n", 0);

	// If the boot setting turned the facility on, every CPU has already armed
	// its sampling counter's overflow (PMINTENSET_EL1) in arm64_pmu_init_percpu().
	// Complete the delivery path here, in normal context and after SMP is up,
	// by installing the PPI handler and enabling the GIC line on every CPU.
	// Guarded internally on sEnabled, so this is a no-op when the PMU is off.
	pmu_install_overflow_interrupt();

	return B_OK;
}
