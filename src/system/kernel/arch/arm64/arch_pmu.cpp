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

	What this deliberately is not:

	- Not a sampling profiler. No overflow interrupt is enabled; we actively
	  mask PMU interrupts because firmware or EL2 may have left them on and
	  the GIC has no handler for the PMU PPI. Sampling would also need to
	  arrive in a context we have not masked interrupts in, which is a much
	  larger design problem.
	- Not a userland interface. There is no syscall, no /dev node, no
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
#include <debug.h>
#include <interrupts.h>
#include <safemode.h>
#include <smp.h>

#include <stdio.h>
#include <string.h>


// The boot setting that turns the facility on for every CPU from early boot.
#define ARM64_PMU_SAFEMODE_OPTION	"arm64_pmu"


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

	// Count cycles in both EL1 and EL0 (all filter bits clear = no inhibit).
	WRITE_SPECIALREG(PMCCFILTR_EL0, 0);

	uint32 count = sEventCount;
	if (count > sCounterCount)
		count = sCounterCount;

	uint32 enableMask = PMU_CYCLE_COUNTER_BIT;
	for (uint32 i = 0; i < count; i++) {
		state.events[i] = sEvents[i];
		state.last[i] = 0;
		state.total[i] = 0;

		pmu_select_counter(i);
		WRITE_SPECIALREG(PMXEVTYPER_EL0,
			(uint64)sEvents[i] & PMEVTYPER_EVTCOUNT_MASK);
		WRITE_SPECIALREG(PMXEVCNTR_EL0, 0);

		enableMask |= PMU_EVENT_COUNTER_BIT(i);
	}

	state.counterCount = count;

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

	state.programmed = true;
}


static void
pmu_stop_cpu(int32 cpu)
{
	WRITE_SPECIALREG(PMCR_EL0, 0);
	WRITE_SPECIALREG(PMCNTENCLR_EL0, PMU_ALL_COUNTERS_MASK);
	WRITE_SPECIALREG(PMOVSCLR_EL0, PMU_ALL_COUNTERS_MASK);
	arm64_isb();

	sPerCPU[cpu].programmed = false;
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


status_t
arm64_pmu_enable(void)
{
	if (!sAvailable)
		return B_NOT_SUPPORTED;

	cpu_status state = disable_interrupts();
	sEnabled = true;
	pmu_program_cpu(smp_get_current_cpu());
	restore_interrupts(state);

	return B_OK;
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
	// next sample's flags mean "wrapped during the last interval".
	uint32 overflow = (uint32)READ_SPECIALREG(PMOVSCLR_EL0);
	if (overflow != 0)
		WRITE_SPECIALREG(PMOVSCLR_EL0, overflow);

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

	sample->wrapped = overflow & ~PMU_CYCLE_COUNTER_BIT;
	sample->valid = true;

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
		"programmed on all CPUs\n", sPerCPU[cpu].counterCount, sCounterCount);

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

	return B_OK;
}
