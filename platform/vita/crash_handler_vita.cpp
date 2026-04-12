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
#include <psp2/rtc.h>

// Note: VitaSDK's dlfcn.h provides dlopen/dlsym/dlclose/dlerror but NOT
// dladdr/Dl_info, so we cannot resolve addresses to symbol names at runtime.
// Use arm-vita-eabi-addr2line offline to resolve addresses from the backtrace.

// Maximum number of stack frames to capture
#define MAX_BACKTRACE_FRAMES 64

// Re-entrancy guard to prevent recursive crashes in the signal handler
static volatile sig_atomic_t s_crash_handler_entered = 0;

// Previous kubridge exception handlers (for chaining)
static KuKernelExceptionHandler s_old_dabt_handler = nullptr;
static KuKernelExceptionHandler s_old_pabt_handler = nullptr;
static KuKernelExceptionHandler s_old_undef_handler = nullptr;

// Pre-allocated crash log directory path, set during initialize()
// so we don't need heap allocation in the signal handler.
static char s_crash_log_dir[256] = { 0 };
static bool s_crash_log_dir_valid = false;

// Manual ARM stack unwinding via frame pointer chain.
// PSVita (ARM Cortex-A9) does not provide execinfo.h / backtrace(),
// so we walk the frame pointer chain manually.
static int vita_backtrace(void **buffer, int max_frames) {
	int count = 0;
	// On ARM, the frame pointer is r11 (fp).
	void **fp = nullptr;

	// Get the current frame pointer using GCC built-in
	fp = (void **)__builtin_frame_address(0);

	while (fp && count < max_frames) {
		// Validate the frame pointer is in a reasonable range
		// (stack on Vita is typically in user address space)
		if ((uintptr_t)fp < 0x1000 || (uintptr_t)fp > 0xFFFFFF00) {
			break;
		}

		// On ARM with frame pointer:
		// fp[0] = previous frame pointer
		// fp[-1] = return address (lr saved by callee)
		// However, the exact layout depends on the ABI and compiler.
		// With GCC on ARM EABI:
		// [fp]     = saved fp (previous frame)
		// [fp - 4] = saved lr (return address)
		void *ret_addr = *((void **)((uintptr_t)fp - sizeof(void *)));
		if (!ret_addr) {
			break;
		}

		buffer[count++] = ret_addr;

		void **next_fp = (void **)*fp;
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

// Open crash log file and write header, returns the file descriptor.
// Uses only stack-allocated buffers and low-level VitaSDK syscalls.
static SceUID _open_crash_log(char *out_path, int out_path_size) {
	if (!s_crash_log_dir_valid) {
		return -1;
	}

	sceIoMkdir(s_crash_log_dir, 0777);

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

	// Engine version
	if (VERSION_HASH && VERSION_HASH[0] != '\0') {
		sceClibSnprintf(buf, sizeof(buf), "Engine version: %s (%s)",
				VERSION_FULL_NAME, VERSION_HASH);
	} else {
		sceClibSnprintf(buf, sizeof(buf), "Engine version: %s", VERSION_FULL_NAME);
	}
	_safe_log(crash_fd, buf);

	// Registers
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

	// Stack-based backtrace using the SP/LR/PC from the exception context.
	// Walk the frame pointer chain starting from R11 (fp) saved in context.
	_safe_log(crash_fd, "Backtrace (from exception context):");
	sceClibSnprintf(buf, sizeof(buf), "  [PC] 0x%08x", ctx->pc);
	_safe_log(crash_fd, buf);
	sceClibSnprintf(buf, sizeof(buf), "  [LR] 0x%08x", ctx->lr);
	_safe_log(crash_fd, buf);

	// Walk frame pointer chain from r11
	uintptr_t fp = ctx->r11;
	int frame = 2;
	while (fp && frame < MAX_BACKTRACE_FRAMES) {
		if (fp < 0x81000000 || fp > 0xBFFFFFFF) {
			break;
		}
		uintptr_t ret_addr = *((uintptr_t *)(fp - 4));
		if (!ret_addr) {
			break;
		}
		sceClibSnprintf(buf, sizeof(buf), "  [%2d] 0x%08x", frame, (unsigned int)ret_addr);
		_safe_log(crash_fd, buf);
		uintptr_t next_fp = *((uintptr_t *)fp);
		if (next_fp <= fp) {
			break;
		}
		fp = next_fp;
		frame++;
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

	// Capture backtrace before doing anything else
	void *bt_buffer[MAX_BACKTRACE_FRAMES];
	int size = vita_backtrace(bt_buffer, MAX_BACKTRACE_FRAMES);

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

	// Engine version
	if (VERSION_HASH && VERSION_HASH[0] != '\0') {
		sceClibSnprintf(buf, sizeof(buf), "Engine version: %s (%s)",
				VERSION_FULL_NAME, VERSION_HASH);
	} else {
		sceClibSnprintf(buf, sizeof(buf), "Engine version: %s", VERSION_FULL_NAME);
	}
	_safe_log(crash_fd, buf);

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

	// Pre-create the default logs directory using low-level API
	sceIoMkdir("ux0:/data/godot_vita", 0777);
	sceIoMkdir("ux0:/data/godot_vita/logs", 0777);

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
			handle_hw_exception, &s_old_dabt_handler, &opt);
	kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
			handle_hw_exception, &s_old_pabt_handler, &opt);
	kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION,
			handle_hw_exception, &s_old_undef_handler, &opt);
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
