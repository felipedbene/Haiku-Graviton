/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <arch/debug.h>

#include <limits.h>

#include <arch_cpu.h>
#include <debug.h>
#include <debug_heap.h>
#include <elf.h>
#include <kernel.h>
#include <kimage.h>
#include <thread.h>
#include <vm/vm.h>
#include <vm/vm_types.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>

#define NUM_PREVIOUS_LOCATIONS 32

extern struct iframe_stack gBootFrameStack;


static bool
already_visited(addr_t* visited, int32* _last, int32* _num, addr_t fp)
{
	int32 last = *_last;
	int32 num = *_num;

	for (int32 i = 0; i < num; i++) {
		if (visited[(NUM_PREVIOUS_LOCATIONS + last - i)
				% NUM_PREVIOUS_LOCATIONS] == fp) {
			return true;
		}
	}

	*_last = last = (last + 1) % NUM_PREVIOUS_LOCATIONS;
	visited[last] = fp;

	if (num < NUM_PREVIOUS_LOCATIONS)
		*_num = num + 1;

	return false;
}


static status_t
get_next_frame(addr_t fp, addr_t *next, addr_t *ip)
{
	if (fp != 0) {
		*ip   = ((addr_t*)fp)[1];
		*next = ((addr_t*)fp)[0];

		return B_OK;
	}

	return B_BAD_VALUE;
}


static bool
is_kernel_stack_address(Thread* thread, addr_t address)
{
	// Early in the boot process there is no thread pointer yet, and a thread
	// may exist before its kernel stack has been assigned; in both cases being
	// a kernel address is the best answer available.
	if (thread == NULL || thread->kernel_stack_base == 0)
		return IS_KERNEL_ADDRESS(address);

	return address >= thread->kernel_stack_base
		&& address < thread->kernel_stack_top;
}


static bool
is_iframe(Thread* thread, addr_t fp)
{
	iframe_stack* frameStack = thread != NULL
		? &thread->arch_info.iframes : &gBootFrameStack;

	for (int32 i = 0; i < frameStack->index; i++) {
		if (fp == (addr_t)frameStack->frames[i])
			return true;
	}

	return false;
}


/*!	Reads the AAPCS64 frame record at \a fp without ever faulting.

	Unlike the KDL walker's get_next_frame(), this is called from a timer
	interrupt on a live system (the system profiler samples from
	SystemProfiler::_DoSample()), so a stale or wild frame pointer -- routine
	when a thread is interrupted mid-prologue, or when the chain runs off the
	end of a stack -- must yield an error rather than a page fault.
*/
static status_t
get_next_frame_no_debugger(addr_t fp, addr_t* _next, addr_t* _ip,
	bool onKernelStack, Thread* thread)
{
	// A frame record is a pair of 64-bit words and the ABI keeps the frame
	// pointer 16-byte aligned, so anything else cannot be one.
	if (fp == 0 || (fp & 0xf) != 0)
		return B_BAD_ADDRESS;

	addr_t frame[2];
	if (onKernelStack
			&& is_kernel_stack_address(thread, fp + sizeof(frame) - 1)) {
		memcpy(frame, (void*)fp, sizeof(frame));
	} else if (!IS_USER_ADDRESS(fp)
			|| user_memcpy(frame, (void*)fp, sizeof(frame)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	*_next = frame[0];
	*_ip = frame[1];

	return B_OK;
}


static status_t
lookup_symbol(Thread* thread, addr_t address, addr_t* _baseAddress,
	const char** _symbolName, const char** _imageName, bool* _exactMatch)
{
	status_t status = B_ENTRY_NOT_FOUND;

	if (address >= KERNEL_BASE) {
		// a kernel symbol
		status = elf_debug_lookup_symbol_address(address, _baseAddress,
			_symbolName, _imageName, _exactMatch);
	} else if (thread != NULL && thread->team != NULL) {
		// try to locate the image in the images loaded into user space
		status = elf_debug_lookup_user_symbol_address(thread->team, address,
			_baseAddress, _symbolName, _imageName, _exactMatch);
	}

	return status;
}


static void
set_debug_argument_variable(int32 index, uint64 value)
{
	char name[8];
	snprintf(name, sizeof(name), "_arg%" B_PRId32, index);
	set_debug_variable(name, value);
}


template<typename Type>
static Type
read_function_argument_value(void* argument, bool& _valueKnown)
{
	Type value;
	if (debug_memcpy(B_CURRENT_TEAM, &value, argument, sizeof(Type)) == B_OK) {
		_valueKnown = true;
		return value;
	}

	_valueKnown = false;
	return 0;
}


static status_t
print_demangled_call(const char* image, const char* symbol, addr_t args,
	bool noObjectMethod, bool addDebugVariables)
{
	static const size_t kBufferSize = 256;
	char* buffer = (char*)debug_malloc(kBufferSize);
	if (buffer == NULL)
		return B_NO_MEMORY;

	bool isObjectMethod;
	const char* name = debug_demangle_symbol(symbol, buffer, kBufferSize,
		&isObjectMethod);
	if (name == NULL) {
		debug_free(buffer);
		return B_ERROR;
	}

	uint32* arg = (uint32*)args;

	if (noObjectMethod)
		isObjectMethod = false;
	if (isObjectMethod) {
		const char* lastName = strrchr(name, ':') - 1;
		int namespaceLength = lastName - name;

		uint32 argValue = 0;
		if (debug_memcpy(B_CURRENT_TEAM, &argValue, arg, 4) == B_OK) {
			kprintf("<%s> %.*s<\33[32m%#" B_PRIx32 "\33[0m>%s", image,
				namespaceLength, name, argValue, lastName);
		} else
			kprintf("<%s> %.*s<\?\?\?>%s", image, namespaceLength, name, lastName);

		if (addDebugVariables)
			set_debug_variable("_this", argValue);
		arg++;
	} else
		kprintf("<%s> %s", image, name);

	kprintf("(");

	size_t length;
	int32 type, i = 0;
	uint32 cookie = 0;
	while (debug_get_next_demangled_argument(&cookie, symbol, buffer,
			kBufferSize, &type, &length) == B_OK) {
		if (i++ > 0)
			kprintf(", ");

		// retrieve value and type identifier

		uint64 value;
		bool valueKnown = false;

		switch (type) {
			case B_INT64_TYPE:
				value = read_function_argument_value<int64>(arg, valueKnown);
				if (valueKnown)
					kprintf("int64: \33[34m%" B_PRId64 "\33[0m", value);
				break;
			case B_INT32_TYPE:
				value = read_function_argument_value<int32>(arg, valueKnown);
				if (valueKnown)
					kprintf("int32: \33[34m%" B_PRId32 "\33[0m", (int32)value);
				break;
			case B_INT16_TYPE:
				value = read_function_argument_value<int16>(arg, valueKnown);
				if (valueKnown)
					kprintf("int16: \33[34m%d\33[0m", (int16)value);
				break;
			case B_INT8_TYPE:
				value = read_function_argument_value<int8>(arg, valueKnown);
				if (valueKnown)
					kprintf("int8: \33[34m%d\33[0m", (int8)value);
				break;
			case B_UINT64_TYPE:
				value = read_function_argument_value<uint64>(arg, valueKnown);
				if (valueKnown) {
					kprintf("uint64: \33[34m%#" B_PRIx64 "\33[0m", value);
					if (value < 0x100000)
						kprintf(" (\33[34m%" B_PRIu64 "\33[0m)", value);
				}
				break;
			case B_UINT32_TYPE:
				value = read_function_argument_value<uint32>(arg, valueKnown);
				if (valueKnown) {
					kprintf("uint32: \33[34m%#" B_PRIx32 "\33[0m", (uint32)value);
					if (value < 0x100000)
						kprintf(" (\33[34m%" B_PRIu32 "\33[0m)", (uint32)value);
				}
				break;
			case B_UINT16_TYPE:
				value = read_function_argument_value<uint16>(arg, valueKnown);
				if (valueKnown) {
					kprintf("uint16: \33[34m%#x\33[0m (\33[34m%u\33[0m)",
						(uint16)value, (uint16)value);
				}
				break;
			case B_UINT8_TYPE:
				value = read_function_argument_value<uint8>(arg, valueKnown);
				if (valueKnown) {
					kprintf("uint8: \33[34m%#x\33[0m (\33[34m%u\33[0m)",
						(uint8)value, (uint8)value);
				}
				break;
			case B_BOOL_TYPE:
				value = read_function_argument_value<uint8>(arg, valueKnown);
				if (valueKnown)
					kprintf("\33[34m%s\33[0m", value ? "true" : "false");
				break;
			default:
				if (buffer[0])
					kprintf("%s: ", buffer);

				if (length == 4) {
					value = read_function_argument_value<uint32>(arg,
						valueKnown);
					if (valueKnown) {
						if (value == 0
							&& (type == B_POINTER_TYPE || type == B_REF_TYPE))
							kprintf("NULL");
						else
							kprintf("\33[34m%#" B_PRIx32 "\33[0m", (uint32)value);
					}
					break;
				}


				if (length == 8) {
					value = read_function_argument_value<uint64>(arg,
						valueKnown);
				} else
					value = (uint64)arg;

				if (valueKnown)
					kprintf("\33[34m%#" B_PRIx64 "\33[0m", value);
				break;
		}

		if (!valueKnown)
			kprintf("???");

		if (valueKnown && type == B_STRING_TYPE) {
			if (value == 0)
				kprintf(" \33[31m\"<NULL>\"\33[0m");
			else if (debug_strlcpy(B_CURRENT_TEAM, buffer, (char*)(addr_t)value,
					kBufferSize) < B_OK) {
				kprintf(" \33[31m\"<\?\?\?>\"\33[0m");
			} else
				kprintf(" \33[36m\"%s\"\33[0m", buffer);
		}

		if (addDebugVariables)
			set_debug_argument_variable(i, value);
		arg = (uint32*)((uint8*)arg + length);
	}

	debug_free(buffer);

	kprintf(")");
	return B_OK;
}


static void
print_stack_frame(Thread* thread, addr_t ip, addr_t calleeFp, addr_t fp,
	int32 callIndex, bool demangle)
{
	const char* symbol;
	const char* image;
	addr_t baseAddress;
	bool exactMatch;
	status_t status;
	addr_t diff;

	diff = fp - calleeFp;

	// kernel space/user space switch
	if (calleeFp > fp)
		diff = 0;

	status = lookup_symbol(thread, ip, &baseAddress, &symbol, &image,
		&exactMatch);

	kprintf("%2" B_PRId32 " %0*lx (+%4ld) %0*lx   ", callIndex,
		B_PRINTF_POINTER_WIDTH, fp, diff, B_PRINTF_POINTER_WIDTH, ip);

	if (status == B_OK) {
		if (exactMatch && demangle) {
			status = print_demangled_call(image, symbol,
				fp, false, false);
		}

		if (!exactMatch || !demangle || status != B_OK) {
			if (symbol != NULL) {
				kprintf("<%s> %s%s", image, symbol,
					exactMatch ? "" : " (nearest)");
			} else
				kprintf("<%s@%p> <unknown>", image, (void*)baseAddress);
		}

		kprintf(" + %#04lx\n", ip - baseAddress);
	} else {
		VMArea *area = NULL;
		if (thread != NULL && thread->team != NULL
			&& thread->team->address_space != NULL) {
			area = thread->team->address_space->LookupArea(ip);
		}
		if (area != NULL) {
			kprintf("%" B_PRId32 ":%s@%p + %#lx\n", area->id, area->name,
				(void*)area->Base(), ip - area->Base());
		} else
			kprintf("\n");
	}
}

static int
stack_trace(int argc, char **argv)
{
	static const char* usage = "usage: %s [-d] [ <thread id> ]\n"
		"Prints a stack trace for the current, respectively the specified\n"
		"thread.\n"
		"  -d           -  Disables the demangling of the symbols.\n"
		"  <thread id>  -  The ID of the thread for which to print the stack\n"
		"                  trace.\n";
	bool demangle = true;
	int32 threadIndex = 1;
	if (argc > 1 && !strcmp(argv[1], "-d")) {
		demangle = false;
		threadIndex++;
	}

	if (argc > threadIndex + 1
		|| (argc == 2 && strcmp(argv[1], "--help") == 0)) {
		kprintf(usage, argv[0]);
		return 0;
	}

	addr_t previousLocations[NUM_PREVIOUS_LOCATIONS];
	Thread* thread = thread_get_current_thread();
	addr_t fp = arm64_get_fp();

	// Honour the thread id this command has always advertised in its usage
	// string. It used to be parsed only for argument counting and then ignored,
	// so `bt <thread>` traced the *calling* thread and printed a plausible,
	// wrong stack -- which is worse than refusing, and defeats the main use of
	// the command: finding out where a blocked thread is parked.
	if (argc == threadIndex + 1) {
		thread_id id = strtoul(argv[threadIndex], NULL, 0);
		Thread* target = Thread::GetDebug(id);
		if (target == NULL) {
			kprintf("could not find thread %" B_PRId32 "\n", id);
			return 0;
		}

		if (id != thread_get_current_thread_id()) {
			if (target->state == B_THREAD_RUNNING) {
				// Running on another CPU, so its saved regs[] are stale; take
				// the frame pointer that CPU recorded on entering KDL instead.
				if (target->cpu == NULL) {
					kprintf("thread %" B_PRId32 " is running but has no cpu\n",
						id);
					return 0;
				}
				arch_debug_registers* registers = debug_get_debug_registers(
					target->cpu->cpu_num);
				if (registers == NULL) {
					kprintf("no debug registers for cpu %d\n",
						target->cpu->cpu_num);
					return 0;
				}
				fp = registers->fp;
			} else {
				// Switched out, so x29 is where _arch_context_swap() put it.
				// arch_thread::regs is x19-x30 then sp, so x29 is index 10.
				fp = target->arch_info.regs[10];
			}

			thread = target;
		}
	}

	int32 num = 0, last = 0;
	struct iframe_stack *frameStack;

	// We don't have a thread pointer early in the boot process
	if (thread != NULL)
		frameStack = &thread->arch_info.iframes;
	else
		frameStack = &gBootFrameStack;

	int32 i;
	for (i = 0; i < frameStack->index; i++) {
		kprintf("iframe %p (end = %p)\n",
			frameStack->frames[i], frameStack->frames[i] + 1);
	}

	if (thread != NULL) {
		kprintf("stack trace for thread 0x%" B_PRIx32 " \"%s\"\n", thread->id,
			thread->name);

		kprintf("    kernel stack: %p to %p\n",
			(void *)thread->kernel_stack_base,
			(void *)(thread->kernel_stack_top));
		if (thread->user_stack_base != 0) {
			kprintf("      user stack: %p to %p\n",
				(void *)thread->user_stack_base,
				(void *)(thread->user_stack_base + thread->user_stack_size));
		}
	}

	kprintf("frame            caller     <image>:function + offset\n");

	for (int32 callIndex = 0;; callIndex++) {
		// see if the frame pointer matches the iframe
		struct iframe *frame = NULL;
		for (i = 0; i < frameStack->index; i++) {
			if (fp == (addr_t)frameStack->frames[i]) {
				// it's an iframe
				frame = frameStack->frames[i];
				break;
			}
		}

		if (frame) {
			kprintf("iframe at %p\n", frame);
			dprintf("ELR=%016lx SPSR=%016lx\n", frame->elr, frame->spsr);
			dprintf("LR =%016lx SP  =%016lx FP =%016lx\n", frame->lr, frame->sp, frame->fp);
			dprintf("ESR=%016lx FAR =%016lx\n", frame->esr, frame->far);
			print_stack_frame(thread, frame->elr, fp, frame->fp, callIndex, demangle);
			fp = frame->fp;
		} else {
			addr_t ip, next;

			if (get_next_frame(fp, &next, &ip) != B_OK) {
				kprintf("%08lx -- read fault\n", fp);
				break;
			}

			if (ip == 0 || fp == 0)
				break;

			print_stack_frame(thread, ip, fp, next, callIndex, demangle);
			fp = next;
		}

		if (already_visited(previousLocations, &last, &num, fp)) {
			kprintf("circular stack frame: %p!\n", (void *)fp);
			break;
		}
		if (fp == 0)
			break;
	}

	return 0;
}


// #pragma mark -


void
arch_debug_save_registers(struct arch_debug_registers* registers)
{
	// Called on each CPU as it enters the kernel debugger, so that a thread
	// which was running elsewhere can still be traced. This was an empty stub,
	// which meant a running thread's frame pointer was whatever the struct
	// happened to contain.
	registers->fp = arm64_get_fp();
}


bool
arch_debug_contains_call(Thread *thread, const char *symbol,
	addr_t start, addr_t end)
{
	return false;
}


void
arch_debug_stack_trace(void)
{
	stack_trace(0, NULL);
}


/*!	Walks the frame-pointer chain of the current thread, recording return
	addresses.

	This is the non-KDL counterpart of stack_trace() above and the hook the
	system profiler samples through (SystemProfiler::_DoSample()), so unlike
	that function it must be safe to run from a timer interrupt on a live
	system and must never fault on a bad frame pointer.

	The walk relies on two arm64 specifics established by EXCEPTION_ENTRY in
	arch_asm.S: the iframe is carved out of the interrupted thread's kernel
	stack, and `mov x29, x0` (arch_asm.S) hands the iframe pointer to the C
	handler as its frame pointer. The chain therefore arrives exactly at an
	iframe address, which is not a frame record -- its first two words are
	`elr` and `spsr` -- so iframes must be recognised and stepped over
	explicitly rather than dereferenced.
*/
int32
arch_debug_get_stack_trace(addr_t* returnAddresses, int32 maxCount,
	int32 skipIframes, int32 skipFrames, uint32 flags)
{
	// Skipping iframes means skipping every frame until the requested number
	// of kernel/user transitions has been crossed; the caller does not know
	// how many normal frames that is.
	if (skipIframes > 0)
		skipFrames = INT_MAX;

	Thread* thread = thread_get_current_thread();
	int32 count = 0;
	addr_t fp = arm64_get_fp();
	bool onKernelStack = true;

	// A cycle in a corrupt chain would otherwise spin forever here, because
	// skipFrames == INT_MAX leaves `count` pinned at 0 and the loop bound with
	// it. Interrupt context is the wrong place to hang.
	int32 iterationsLeft = maxCount + 2 * IFRAME_TRACE_DEPTH + 32;

	while (fp != 0 && count < maxCount && iterationsLeft-- > 0) {
		onKernelStack = onKernelStack && is_kernel_stack_address(thread, fp);
		if (!onKernelStack && (flags & STACK_TRACE_USER) == 0)
			break;

		addr_t ip;
		addr_t nextFp;

		if (onKernelStack && is_iframe(thread, fp)) {
			iframe* frame = (iframe*)fp;
			ip = frame->elr;
			nextFp = frame->fp;

			if (skipIframes > 0) {
				if (--skipIframes == 0)
					skipFrames = 0;
			}
		} else {
			if (get_next_frame_no_debugger(fp, &nextFp, &ip, onKernelStack,
					thread) != B_OK) {
				break;
			}
		}

		if (ip == 0)
			break;

		if (skipFrames > 0)
			skipFrames--;
		else
			returnAddresses[count++] = ip;

		fp = nextFp;
	}

	return count;
}


void*
arch_debug_get_interrupt_pc(bool* _isSyscall)
{
	Thread* thread = debug_get_debugged_thread();
	iframe_stack* frameStack = thread != NULL
		? &thread->arch_info.iframes : &gBootFrameStack;

	if (frameStack->index <= 0)
		return NULL;

	iframe* frame = frameStack->frames[frameStack->index - 1];

	if (_isSyscall != NULL) {
		// ESR_EL1 exception class 0x15 is "SVC instruction execution in
		// AArch64 state", which is how a Haiku syscall enters the kernel.
		*_isSyscall = ((frame->esr >> 26) & 0x3f) == 0x15;
	}

	return (void*)(addr_t)frame->elr;
}


bool
arch_is_debug_variable_defined(const char* variableName)
{
	return false;
}


status_t
arch_set_debug_variable(const char* variableName, uint64 value)
{
	return B_ENTRY_NOT_FOUND;
}


status_t
arch_get_debug_variable(const char* variableName, uint64* value)
{
	return B_ENTRY_NOT_FOUND;
}


void
arch_debug_snooze(bigtime_t duration)
{
	spin(duration);
}


status_t
arch_debug_init(kernel_args *args)
{
	add_debugger_command("where", &stack_trace, "Same as \"sc\"");
	add_debugger_command("bt", &stack_trace, "Same as \"sc\" (as in gdb)");
	add_debugger_command("sc", &stack_trace, "Stack crawl for current thread");

	return B_NO_ERROR;
}


void
arch_debug_unset_current_thread(void)
{
}


ssize_t
arch_debug_gdb_get_registers(char* buffer, size_t bufferSize)
{
	return B_NOT_SUPPORTED;
}
