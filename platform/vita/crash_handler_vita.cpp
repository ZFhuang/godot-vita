/**************************************************************************/
/*  crash_handler_vita.cpp                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "crash_handler_vita.h"

#include "core/os/dir_access.h"
#include "core/os/os.h"
#include "core/project_settings.h"
#include "core/version.h"
#include "main/main.h"

#ifdef DEBUG_ENABLED
#define CRASH_HANDLER_ENABLED 1
#endif

#ifdef CRASH_HANDLER_ENABLED
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>

// Note: VitaSDK's dlfcn.h provides dlopen/dlsym/dlclose/dlerror but NOT
// dladdr/Dl_info, so we cannot resolve addresses to symbol names at runtime.
// Use arm-vita-eabi-addr2line offline to resolve addresses from the backtrace.

// Maximum number of stack frames to capture
#define MAX_BACKTRACE_FRAMES 64

// Maximum number of candidate return addresses from stack scanning
#define MAX_STACK_SCAN_ADDRS 48

// Stack scan range in words (each word = 4 bytes, 1024 words = 4KB)
#define STACK_SCAN_WORDS 1024

// Re-entrancy guard to prevent recursive crashes in the signal handler
static volatile sig_atomic_t s_crash_handler_entered = 0;

// Pre-allocated crash log directory path, set during initialize()
// so we don't need heap allocation in the signal handler.
static char s_crash_log_dir[256] = { 0 };
static bool s_crash_log_dir_valid = false;

// Heuristic stack scanner: scan the stack for values that look like code addresses.
// This mimics what analyze_crash.py does for .psp2dmp files, but runs on-device.
//
// For Vita homebrew EXEC ELFs, the executable code (RX LOAD segment) is always
// loaded at a fixed base address of 0x81000000. We use the linker-provided
// symbols _init (start of .init) and _fini (end of .fini) to determine the
// exact code range, falling back to a conservative estimate if unavailable.
//
// Parameters:
//   sp         - stack pointer at crash time
//   pc         - program counter at crash time (excluded from results)
//   lr         - link register at crash time (excluded from results)
//   buffer     - output array for candidate return addresses
//   max_addrs  - capacity of buffer
// Returns the number of candidate addresses found.

// Linker-provided symbols marking the boundaries of executable code.
extern "C" {
extern char _init;   // Start of .init section (first executable code)
extern char _fini;   // Start of .fini section
}

static int _scan_stack_for_code_addrs(uintptr_t sp, uintptr_t pc, uintptr_t lr,
		void **buffer, int max_addrs) {
	// Determine code range from linker symbols.
	// _init is at the very start of executable code (0x81000000).
	// _fini is the start of .fini section; add a generous margin for .fini + stubs.
	uintptr_t code_start = (uintptr_t)&_init;
	uintptr_t code_end = (uintptr_t)&_fini + 0x10000; // Conservative upper bound

	// Sanity check: if linker symbols look wrong, use hardcoded range
	if (code_start < 0x81000000 || code_start > 0x82FFFFFF ||
			code_end < code_start || code_end > 0x82FFFFFF) {
		code_start = 0x81000000;
		code_end = 0x82900000;
	}

	int count = 0;
	uintptr_t pc_clean = pc & ~1; // Clear Thumb bit
	uintptr_t lr_clean = lr & ~1;

	for (int i = 0; i < STACK_SCAN_WORDS && count < max_addrs; i++) {
		uintptr_t addr = sp + i * 4;

		// Safety: don't read beyond reasonable stack bounds
		if (addr < 0x81000000 || addr > 0xBFFFFFFF) {
			break;
		}

		uintptr_t val = *((volatile uintptr_t *)addr);
		if (val == 0) {
			continue;
		}

		uintptr_t val_clean = val & ~1; // Clear Thumb bit

		// Check if this value falls within the executable code range
		if (val_clean >= code_start && val_clean < code_end) {
			// Skip PC and LR (already reported separately)
			if (val_clean == pc_clean || val_clean == lr_clean) {
				continue;
			}

			// Deduplicate: skip if we already have this address
			bool dup = false;
			for (int j = 0; j < count; j++) {
				if (((uintptr_t)buffer[j] & ~1) == val_clean) {
					dup = true;
					break;
				}
			}
			if (!dup) {
				buffer[count++] = (void *)val;
			}
		}
	}

	return count;
}

// Unified ARM frame pointer chain walker.
// PSVita (ARM Cortex-A9) does not provide execinfo.h / backtrace(),
// so we walk the frame pointer chain manually.
// Parameters:
//   start_fp   - initial frame pointer value (r11)
//   fp_lo/fp_hi - valid address range for frame pointers
//   buffer     - output array for return addresses
//   max_frames - capacity of buffer
// Returns the number of frames captured.
static int _walk_frame_chain(uintptr_t start_fp, uintptr_t fp_lo, uintptr_t fp_hi,
		void **buffer, int max_frames) {
	int count = 0;
	uintptr_t fp = start_fp;

	while (fp && count < max_frames) {
		if (fp < fp_lo || fp > fp_hi) {
			break;
		}

		// ARM EABI frame layout:
		// [fp]     = saved fp (previous frame)
		// [fp - 4] = saved lr (return address)
		uintptr_t ret_addr = *((uintptr_t *)(fp - sizeof(void *)));
		if (!ret_addr) {
			break;
		}

		buffer[count++] = (void *)ret_addr;

		uintptr_t next_fp = *((uintptr_t *)fp);
		// Ensure we're moving up the stack (prevent infinite loops)
		if (next_fp <= fp) {
			break;
		}
		fp = next_fp;
	}

	return count;
}

static const char *_get_signal_name(int sig) {
	switch (sig) {
		case SIGSEGV: return "SIGSEGV (Segmentation fault)";
		case SIGFPE: return "SIGFPE (Floating-point exception)";
		case SIGILL: return "SIGILL (Illegal instruction)";
		case SIGABRT: return "SIGABRT (Aborted)";
		case SIGBUS: return "SIGBUS (Bus error)";
		default: return "Unknown signal";
	}
}

static const char *_get_exception_type_name(SceUInt32 type) {
	switch (type) {
		case KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT: return "Data Abort";
		case KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT: return "Prefetch Abort";
		case KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION: return "Undefined Instruction";
		default: return "Unknown Exception";
	}
}

// Decode ARM Data Fault Status Register (DFSR) / Instruction Fault Status Register (IFSR)
// into a human-readable fault type string.
// FSR encoding: bits [10, 3:0] form a 5-bit fault status on ARMv7.
static const char *_decode_fsr(SceUInt32 fsr) {
	// Extract fault status: FS[4] = bit 10, FS[3:0] = bits 3:0
	SceUInt32 fs = ((fsr >> 10) & 0x1) << 4 | (fsr & 0xF);
	switch (fs) {
		case 0x01: return "Alignment fault";
		case 0x02: return "Debug event";
		case 0x03: return "Access flag fault (Section)";
		case 0x04: return "Instruction cache maintenance fault";
		case 0x05: return "Translation fault (Section)";
		case 0x06: return "Access flag fault (Page)";
		case 0x07: return "Translation fault (Page)";
		case 0x08: return "Synchronous external abort (non-translation)";
		case 0x09: return "Domain fault (Section)";
		case 0x0B: return "Domain fault (Page)";
		case 0x0C: return "Synchronous external abort on translation (1st level)";
		case 0x0D: return "Permission fault (Section)";
		case 0x0E: return "Synchronous external abort on translation (2nd level)";
		case 0x0F: return "Permission fault (Page)";
		case 0x10: return "TLB conflict abort";
		case 0x16: return "Asynchronous external abort";
		case 0x19: return "Synchronous parity error on memory access";
		case 0x1C: return "Synchronous parity error on translation (1st level)";
		case 0x1E: return "Synchronous parity error on translation (2nd level)";
		default: return "Unknown fault type";
	}
}

// Open crash log file and write header, returns the file descriptor.
// Uses only stack-allocated buffers and low-level VitaSDK syscalls.
// Note: The crash log directory is pre-created in initialize() and
// setup_crash_log_dir(), so no sceIoMkdir call is needed here.
static SceUID _open_crash_log(char *out_path, int out_path_size) {
	if (!s_crash_log_dir_valid) {
		return -1;
	}

	SceDateTime dt;
	sceClibMemset(&dt, 0, sizeof(dt));
	if (sceRtcGetCurrentClockLocalTime(&dt) < 0) {
		dt.year = 0;
		dt.month = 0;
		dt.day = 0;
		dt.hour = 0;
		dt.minute = 0;
		dt.second = 0;
	}

	sceClibSnprintf(out_path, out_path_size,
			"%s/crash_%04d%02d%02d_%02d%02d%02d.log",
			s_crash_log_dir,
			dt.year, dt.month, dt.day,
			dt.hour, dt.minute, dt.second);

	return sceIoOpen(out_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

// Async-signal-safe helper: write a C string to the crash log file descriptor.
// Uses only sceIoWrite (low-level I/O), no heap allocation.
static void _safe_write_line(SceUID fd, const char *line) {
	if (fd < 0 || !line) {
		return;
	}
	int len = 0;
	while (line[len] != '\0') {
		len++;
	}
	if (len > 0) {
		sceIoWrite(fd, line, len);
	}
	sceIoWrite(fd, "\n", 1);
}

// Async-signal-safe helper: write to both sceClibPrintf (console) and file.
static void _safe_log(SceUID fd, const char *line) {
	sceClibPrintf("%s\n", line);
	_safe_write_line(fd, line);
}

// Async-signal-safe helper: log engine version info.
static void _log_engine_version(SceUID fd, char *buf, int buf_size) {
	if (VERSION_HASH && VERSION_HASH[0] != '\0') {
		sceClibSnprintf(buf, buf_size, "Engine version: %s (%s)",
				VERSION_FULL_NAME, VERSION_HASH);
	} else {
		sceClibSnprintf(buf, buf_size, "Engine version: %s", VERSION_FULL_NAME);
	}
	_safe_log(fd, buf);
}

// Log current thread information: thread name, ID, priority, stack free size.
static void _log_thread_info(SceUID fd, char *buf, int buf_size) {
	_safe_log(fd, "Thread info:");

	SceUID thid = sceKernelGetThreadId();
	if (thid < 0) {
		_safe_log(fd, "  (failed to get thread ID)");
		return;
	}

	SceKernelThreadInfo tinfo;
	sceClibMemset(&tinfo, 0, sizeof(tinfo));
	tinfo.size = sizeof(tinfo);

	int stack_size = 0;
	if (sceKernelGetThreadInfo(thid, &tinfo) >= 0) {
		stack_size = tinfo.stackSize;
		sceClibSnprintf(buf, buf_size,
				"  Name: %s  TID: 0x%08x  Priority: %d  CPU: %d",
				tinfo.name, thid, tinfo.currentPriority, tinfo.currentCpuId);
		_safe_log(fd, buf);
		sceClibSnprintf(buf, buf_size,
				"  Stack size: %d bytes",
				stack_size);
		_safe_log(fd, buf);
	} else {
		sceClibSnprintf(buf, buf_size, "  TID: 0x%08x (failed to get details)", thid);
		_safe_log(fd, buf);
	}

	int stack_free = sceKernelGetThreadStackFreeSize(thid);
	if (stack_free >= 0) {
		// In exception handler context, sceKernelGetThreadStackFreeSize may
		// return unreliable values (e.g. larger than stack size).
		const char *warning = "";
		if (stack_size > 0 && stack_free > stack_size) {
			warning = " (unreliable - in exception context)";
		} else if (stack_free < 1024) {
			warning = " ** VERY LOW - possible stack overflow! **";
		}
		sceClibSnprintf(buf, buf_size, "  Stack free: %d bytes%s",
				stack_free, warning);
		_safe_log(fd, buf);
	}
}

// Log free memory information for different memory types.
// Note: sceClibSnprintf does NOT support %f (floating-point format specifiers).
// We compute MB using integer arithmetic: whole part and 2-digit fractional part.
static void _log_memory_info(SceUID fd, char *buf, int buf_size) {
	_safe_log(fd, "Memory info:");

	SceKernelFreeMemorySizeInfo mem_info;
	sceClibMemset(&mem_info, 0, sizeof(mem_info));
	mem_info.size = sizeof(mem_info);

	if (sceKernelGetFreeMemorySize(&mem_info) >= 0) {
		// Compute MB with 2 decimal places using integer math:
		// mb_whole = bytes / (1024*1024)
		// mb_frac  = (bytes % (1024*1024)) * 100 / (1024*1024)
		unsigned int user_mb = mem_info.size_user / (1024 * 1024);
		unsigned int user_frac = (mem_info.size_user % (1024 * 1024)) * 100 / (1024 * 1024);
		sceClibSnprintf(buf, buf_size,
				"  User RAM free:  %u bytes (%u.%02u MB)",
				(unsigned int)mem_info.size_user, user_mb, user_frac);
		_safe_log(fd, buf);

		unsigned int cdram_mb = mem_info.size_cdram / (1024 * 1024);
		unsigned int cdram_frac = (mem_info.size_cdram % (1024 * 1024)) * 100 / (1024 * 1024);
		sceClibSnprintf(buf, buf_size,
				"  CDRAM free:     %u bytes (%u.%02u MB)",
				(unsigned int)mem_info.size_cdram, cdram_mb, cdram_frac);
		_safe_log(fd, buf);

		unsigned int phycont_mb = mem_info.size_phycont / (1024 * 1024);
		unsigned int phycont_frac = (mem_info.size_phycont % (1024 * 1024)) * 100 / (1024 * 1024);
		sceClibSnprintf(buf, buf_size,
				"  Phycont free:   %u bytes (%u.%02u MB)",
				(unsigned int)mem_info.size_phycont, phycont_mb, phycont_frac);
		_safe_log(fd, buf);

		// Warn if memory is critically low
		if (mem_info.size_user < 1024 * 1024) {
			_safe_log(fd, "  ** WARNING: User RAM critically low - possible out-of-memory! **");
		}
	} else {
		_safe_log(fd, "  (failed to get memory info)");
	}
}

// Log process uptime at the time of crash.
// Note: sceClibSnprintf does NOT support %llu (64-bit format specifiers).
// We must cast down to 32-bit values. Process uptime in practice will never
// exceed 2^32 seconds (~136 years), so this is safe.
static void _log_process_uptime(SceUID fd, char *buf, int buf_size) {
	SceUInt64 proc_time = sceKernelGetProcessTimeWide(); // microseconds
	SceUInt32 total_sec = (SceUInt32)(proc_time / 1000000ULL);
	SceUInt32 ms = (SceUInt32)((proc_time / 1000ULL) % 1000ULL);
	SceUInt32 hours = total_sec / 3600;
	SceUInt32 minutes = (total_sec % 3600) / 60;
	SceUInt32 seconds = total_sec % 60;
	sceClibSnprintf(buf, buf_size,
			"Process uptime: %u:%02u:%02u (%u.%03u seconds)",
			(unsigned int)hours, (unsigned int)minutes, (unsigned int)seconds,
			(unsigned int)total_sec, (unsigned int)ms);
	_safe_log(fd, buf);
}

// kubridge hardware exception handler.
// Called directly by the kernel on Data abort / Prefetch abort / Undefined instruction.
// Constraints: no heap allocation, no POSIX calls, only sceIo* and sceClibPrintf.
static void handle_hw_exception(KuKernelExceptionContext *ctx) {
	// Re-entrancy guard
	if (s_crash_handler_entered) {
		_Exit(1);
	}
	s_crash_handler_entered = 1;

	char buf[512];
	char crash_log_path[256] = { 0 };
	SceUID crash_fd = _open_crash_log(crash_log_path, sizeof(crash_log_path));

	const char *separator = "================================================================";
	_safe_log(crash_fd, separator);

	sceClibSnprintf(buf, sizeof(buf), "CRASH: Hardware exception - %s",
			_get_exception_type_name(ctx->exceptionType));
	_safe_log(crash_fd, buf);
	_log_engine_version(crash_fd, buf, sizeof(buf));

	// Registers
	_log_process_uptime(crash_fd, buf, sizeof(buf));
	_log_thread_info(crash_fd, buf, sizeof(buf));
	_log_memory_info(crash_fd, buf, sizeof(buf));

	_safe_log(crash_fd, "Registers:");
	sceClibSnprintf(buf, sizeof(buf),
			"  R0=0x%08x  R1=0x%08x  R2=0x%08x  R3=0x%08x",
			ctx->r0, ctx->r1, ctx->r2, ctx->r3);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf),
			"  R4=0x%08x  R5=0x%08x  R6=0x%08x  R7=0x%08x",
			ctx->r4, ctx->r5, ctx->r6, ctx->r7);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf),
			"  R8=0x%08x  R9=0x%08x R10=0x%08x R11=0x%08x",
			ctx->r8, ctx->r9, ctx->r10, ctx->r11);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf),
			" R12=0x%08x  SP=0x%08x  LR=0x%08x  PC=0x%08x",
			ctx->r12, ctx->sp, ctx->lr, ctx->pc);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf),
			" SPSR=0x%08x  FSR=0x%08x  FAR=0x%08x",
			ctx->SPSR, ctx->FSR, ctx->FAR);
	_safe_log(crash_fd, buf);

	// Decode FSR into human-readable fault type
	if (ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT ||
			ctx->exceptionType == KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT) {
		sceClibSnprintf(buf, sizeof(buf), "  Fault type: %s", _decode_fsr(ctx->FSR));
		_safe_log(crash_fd, buf);
	}

	// Backtrace using the SP/LR/PC from the exception context.
	// Walk the frame pointer chain starting from R11 (fp) saved in context.
	_safe_log(crash_fd, "Backtrace (from exception context):");
	sceClibSnprintf(buf, sizeof(buf), "  [PC] 0x%08x", ctx->pc);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf), "  [LR] 0x%08x", ctx->lr);
	_safe_log(crash_fd, buf);

	// Walk frame pointer chain from r11 using the unified walker.
	// Vita user-space stack addresses are in the range 0x81000000-0xBFFFFFFF.
	void *bt_buffer[MAX_BACKTRACE_FRAMES];
	int bt_count = _walk_frame_chain(ctx->r11, 0x81000000, 0xBFFFFFFF,
			bt_buffer, MAX_BACKTRACE_FRAMES);
	for (int i = 0; i < bt_count; i++) {
		sceClibSnprintf(buf, sizeof(buf), "  [%2d] 0x%08x",
				i + 2, (unsigned int)(uintptr_t)bt_buffer[i]);
		_safe_log(crash_fd, buf);
	}

	// Heuristic stack scan: find candidate return addresses by scanning
	// the stack for values that point into executable code.
	// This provides deeper call chain info even without frame pointers.
	void *scan_buffer[MAX_STACK_SCAN_ADDRS];
	int scan_count = _scan_stack_for_code_addrs(ctx->sp, ctx->pc, ctx->lr,
			scan_buffer, MAX_STACK_SCAN_ADDRS);
	if (scan_count > 0) {
		_safe_log(crash_fd, "Stack scan (heuristic, may contain false positives):");
		for (int i = 0; i < scan_count; i++) {
			sceClibSnprintf(buf, sizeof(buf), "  [S%2d] 0x%08x",
					i, (unsigned int)(uintptr_t)scan_buffer[i]);
			_safe_log(crash_fd, buf);
		}
	}

	_safe_log(crash_fd,
			"Hint: Use arm-vita-eabi-addr2line -e <elf> -f -C <addresses> to resolve symbols.");
	_safe_log(crash_fd, "-- END OF BACKTRACE --");

	if (crash_fd >= 0 && crash_log_path[0] != '\0') {
		sceClibSnprintf(buf, sizeof(buf), "Crash log saved to: %s", crash_log_path);
		_safe_log(crash_fd, buf);
	}
	_safe_log(crash_fd, separator);

	if (crash_fd >= 0) {
		sceIoClose(crash_fd);
	}

	// Do NOT call _Exit here - return SCE_EXCPMGR_EXCEPTION_NOT_HANDLED
	// so the system still generates the .psp2dmp file.
	// The handler returns void; the not-handled path is taken by default
	// when we don't call kuKernelReleaseExceptionHandler.
}

static void handle_crash(int sig) {
	// Re-entrancy guard: if we crash again inside the handler, just die immediately.
	if (s_crash_handler_entered) {
		_Exit(128 + sig);
	}
	s_crash_handler_entered = 1;

	// Block all crash signals to prevent recursive signal delivery
	signal(SIGSEGV, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGBUS, SIG_DFL);

	// All operations below use only stack-allocated buffers and low-level
	// VitaSDK syscalls to remain async-signal-safe. No Godot String,
	// FileAccess, DirAccess, or any heap-allocating API is used.

	char buf[512];

	// Open crash log file using low-level I/O (no heap allocation)
	SceUID crash_fd = -1;
	char crash_log_path[256] = { 0 };
	crash_fd = _open_crash_log(crash_log_path, sizeof(crash_log_path));

	// Capture backtrace via frame pointer chain.
	// Note: In a signal handler, __builtin_frame_address(0) gives us the
	// signal handler's own stack frame, which may not connect back to the
	// crash site. The backtrace may be incomplete or empty.
	void *bt_buffer[MAX_BACKTRACE_FRAMES];
	uintptr_t current_fp = (uintptr_t)__builtin_frame_address(0);
	int size = _walk_frame_chain(current_fp, 0x81000000, 0xBFFFFFFF,
			bt_buffer, MAX_BACKTRACE_FRAMES);

	// Notify MainLoop about the crash (this is the only Godot API call we keep,
	// as it's important for user-side crash handling in Node scripts).
	if (OS::get_singleton() && OS::get_singleton()->get_main_loop()) {
		OS::get_singleton()->get_main_loop()->notification(MainLoop::NOTIFICATION_CRASH);
	}

	// Output crash information
	const char *separator = "================================================================";
	_safe_log(crash_fd, separator);

	sceClibSnprintf(buf, sizeof(buf), "CRASH: Program crashed with signal %d - %s",
			sig, _get_signal_name(sig));
	_safe_log(crash_fd, buf);
	_log_engine_version(crash_fd, buf, sizeof(buf));
	_log_process_uptime(crash_fd, buf, sizeof(buf));
	_log_thread_info(crash_fd, buf, sizeof(buf));
	_log_memory_info(crash_fd, buf, sizeof(buf));

	_safe_log(crash_fd, "Dumping the backtrace.");

	if (size > 0) {
		for (int i = 0; i < size; i++) {
			sceClibSnprintf(buf, sizeof(buf), "[%d] [0x%08x]",
					i, (unsigned int)(uintptr_t)bt_buffer[i]);
			_safe_log(crash_fd, buf);
		}
		_safe_log(crash_fd,
				"Hint: Use arm-vita-eabi-addr2line -e <elf> -f -C <addresses> to resolve symbols.");
	} else {
		_safe_log(crash_fd, "No backtrace frames could be captured.");
		_safe_log(crash_fd,
				"Hint: Ensure the build uses -fno-omit-frame-pointer for better stack traces.");
	}

	// Heuristic stack scan for signal handler.
	// Use the current SP since we don't have the crash-site SP in signal context.
	uintptr_t signal_sp;
	__asm__ volatile("mov %0, sp" : "=r"(signal_sp));
	void *scan_buffer[MAX_STACK_SCAN_ADDRS];
	int scan_count = _scan_stack_for_code_addrs(signal_sp, 0, 0,
			scan_buffer, MAX_STACK_SCAN_ADDRS);
	if (scan_count > 0) {
		_safe_log(crash_fd, "Stack scan (heuristic, may contain false positives):");
		for (int i = 0; i < scan_count; i++) {
			sceClibSnprintf(buf, sizeof(buf), "  [S%2d] 0x%08x",
					i, (unsigned int)(uintptr_t)scan_buffer[i]);
			_safe_log(crash_fd, buf);
		}
	}

	_safe_log(crash_fd, "-- END OF BACKTRACE --");

	// Write crash log path
	if (crash_fd >= 0 && crash_log_path[0] != '\0') {
		sceClibSnprintf(buf, sizeof(buf), "Crash log saved to: %s", crash_log_path);
		_safe_log(crash_fd, buf);
	}

	_safe_log(crash_fd, separator);

	// Close crash log file
	if (crash_fd >= 0) {
		sceIoClose(crash_fd);
	}

	// Use _Exit() instead of abort() to avoid triggering SIGABRT recursion
	_Exit(128 + sig);
}
#endif

CrashHandler::CrashHandler() {
	disabled = false;
}

CrashHandler::~CrashHandler() {
	disable();
}

void CrashHandler::disable() {
	if (disabled) {
		return;
	}

#ifdef CRASH_HANDLER_ENABLED
	signal(SIGSEGV, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGBUS, SIG_DFL);

	// Release kubridge hardware exception handlers
	kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT);
	kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT);
	kuKernelReleaseExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION);
#endif

	disabled = true;
}

void CrashHandler::initialize() {
#ifdef CRASH_HANDLER_ENABLED
	// Use a hardcoded default crash log directory.
	// At this point in the boot sequence, ProjectSettings is NOT yet initialized
	// (initialize_core() is called before memnew(ProjectSettings) in Main::setup()),
	// so we cannot call get_user_data_dir() which depends on ProjectSettings.
	// Use a safe default path that doesn't require any Godot singletons.
	strncpy(s_crash_log_dir, "ux0:/data/godot_vita/logs", sizeof(s_crash_log_dir) - 1);
	s_crash_log_dir[sizeof(s_crash_log_dir) - 1] = '\0';
	s_crash_log_dir_valid = true;

	signal(SIGSEGV, handle_crash);
	signal(SIGFPE, handle_crash);
	signal(SIGILL, handle_crash);
	signal(SIGABRT, handle_crash);
	signal(SIGBUS, handle_crash);

	// Register kubridge hardware exception handlers to catch Data abort,
	// Prefetch abort and Undefined instruction - these are ARM CPU-level
	// exceptions that bypass POSIX signals on PSVita.
	KuKernelExceptionHandlerOpt opt;
	sceClibMemset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT,
			handle_hw_exception, nullptr, &opt);
	kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
			handle_hw_exception, nullptr, &opt);
	kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION,
			handle_hw_exception, nullptr, &opt);
#endif
}

void CrashHandler::setup_crash_log_dir() {
#ifdef CRASH_HANDLER_ENABLED
	// Called after ProjectSettings is initialized, so we can now resolve
	// the proper user data directory for crash logs.
	if (OS::get_singleton()) {
		String user_dir = OS::get_singleton()->get_user_data_dir();
		if (!user_dir.empty()) {
			String logs_dir = user_dir.plus_file("logs");
			if (logs_dir.utf8().length() < (int)sizeof(s_crash_log_dir)) {
				strncpy(s_crash_log_dir, logs_dir.utf8().get_data(), sizeof(s_crash_log_dir) - 1);
				s_crash_log_dir[sizeof(s_crash_log_dir) - 1] = '\0';
				s_crash_log_dir_valid = true;

				// Pre-create the logs directory
				DirAccess *dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
				if (dir) {
					dir->make_dir_recursive(logs_dir);
					memdelete(dir);
				}
			}
		}
	}
#endif
}
