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
#include "core/os/file_access.h"
#include "core/os/os.h"
#include "core/print_string.h"
#include "core/project_settings.h"
#include "core/version.h"
#include "main/main.h"

#ifdef DEBUG_ENABLED
#define CRASH_HANDLER_ENABLED 1
#endif

#ifdef CRASH_HANDLER_ENABLED
#include <signal.h>
#include <stdlib.h>
#include <time.h>

#include <psp2/kernel/clib.h>

// Note: VitaSDK's dlfcn.h provides dlopen/dlsym/dlclose/dlerror but NOT
// dladdr/Dl_info, so we cannot resolve addresses to symbol names at runtime.
// Use arm-vita-eabi-addr2line offline to resolve addresses from the backtrace.

// Maximum number of stack frames to capture
#define MAX_BACKTRACE_FRAMES 64

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

// Helper to write crash log to file
static void _write_crash_log_line(FileAccess *p_file, const String &p_line) {
	if (p_file) {
		p_file->store_string(p_line + "\n");
		p_file->flush();
	}
}

static void handle_crash(int sig) {
	if (OS::get_singleton() == nullptr) {
		abort();
	}

	// Open crash log file
	FileAccess *crash_log = nullptr;
	String crash_log_path;
	{
		// Use user data directory for crash logs
		String user_dir = OS::get_singleton()->get_user_data_dir();
		if (!user_dir.empty()) {
			// Ensure logs directory exists
			DirAccess *dir = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
			if (dir) {
				dir->make_dir_recursive(user_dir.plus_file("logs"));
				memdelete(dir);
			}

			// Create crash log filename with timestamp
			time_t now = time(nullptr);
			struct tm *timeinfo = localtime(&now);
			char timestamp[32];
			strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", timeinfo);

			crash_log_path = user_dir.plus_file("logs").plus_file(vformat("crash_%s.log", timestamp));
			crash_log = FileAccess::open(crash_log_path, FileAccess::WRITE);
		}
	}

	void *bt_buffer[MAX_BACKTRACE_FRAMES];
	int size = vita_backtrace(bt_buffer, MAX_BACKTRACE_FRAMES);

	String msg;
	const ProjectSettings *proj_settings = ProjectSettings::get_singleton();
	if (proj_settings) {
		msg = proj_settings->get("debug/settings/crash_handler/message");
	}

	// Tell MainLoop about the crash. This can be handled by users too in Node.
	if (OS::get_singleton()->get_main_loop()) {
		OS::get_singleton()->get_main_loop()->notification(MainLoop::NOTIFICATION_CRASH);
	}

	// Dump the backtrace to stderr with a message to the user
	// Use sceClibPrintf as a fallback in case print_error has issues during crash
	sceClibPrintf("\n================================================================\n");
	sceClibPrintf("CRASH: Program crashed with signal %d - %s\n", sig, _get_signal_name(sig));

	String separator = "================================================================";
	String crash_header = vformat("CRASH: Program crashed with signal %d - %s", sig, _get_signal_name(sig));

	print_error("\n" + separator);
	_write_crash_log_line(crash_log, separator);

	print_error(vformat("%s: %s", __FUNCTION__, crash_header));
	_write_crash_log_line(crash_log, crash_header);

	// Print the engine version
	String version_str;
	if (String(VERSION_HASH).empty()) {
		version_str = vformat("Engine version: %s", VERSION_FULL_NAME);
	} else {
		version_str = vformat("Engine version: %s (%s)", VERSION_FULL_NAME, VERSION_HASH);
	}
	print_error(version_str);
	_write_crash_log_line(crash_log, version_str);

	String dump_msg = vformat("Dumping the backtrace. %s", msg);
	print_error(dump_msg);
	_write_crash_log_line(crash_log, dump_msg);

	if (size > 0) {
		for (int i = 0; i < size; i++) {
			// VitaSDK lacks dladdr/Dl_info, so we output raw addresses only.
			// Use arm-vita-eabi-addr2line -e <elf> -f -C <addresses> to resolve symbols offline.
			sceClibPrintf("[%d] [%p]\n", i, bt_buffer[i]);
			String bt_line = vformat("[%d] [0x%x]", (int64_t)i, (int64_t)(uintptr_t)bt_buffer[i]);
			print_error(bt_line);
			_write_crash_log_line(crash_log, bt_line);
		}
		String hint = "Hint: Use arm-vita-eabi-addr2line -e <elf> -f -C <addresses> to resolve symbols.";
		print_error(hint);
		_write_crash_log_line(crash_log, hint);
		sceClibPrintf("Hint: Use arm-vita-eabi-addr2line -e <elf> -f -C <addresses> to resolve symbols.\n");
	} else {
		String no_bt = "No backtrace frames could be captured.";
		String hint = "Hint: Ensure the build uses -fno-omit-frame-pointer for better stack traces.";
		print_error(no_bt);
		print_error(hint);
		_write_crash_log_line(crash_log, no_bt);
		_write_crash_log_line(crash_log, hint);
		sceClibPrintf("No backtrace frames could be captured.\n");
	}

	String end_marker = "-- END OF BACKTRACE --";
	print_error(end_marker);
	print_error(separator);
	_write_crash_log_line(crash_log, end_marker);
	_write_crash_log_line(crash_log, separator);

	// Write crash log path to console
	if (!crash_log_path.empty()) {
		String saved_msg = vformat("Crash log saved to: %s", crash_log_path);
		print_error(saved_msg);
		sceClibPrintf("%s\n", saved_msg.utf8().get_data());
		_write_crash_log_line(crash_log, saved_msg);
	}

	sceClibPrintf("-- END OF BACKTRACE --\n");
	sceClibPrintf("================================================================\n");

	// Close crash log file
	if (crash_log) {
		memdelete(crash_log);
	}

	// Abort to pass the error to the OS
	abort();
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
	signal(SIGSEGV, nullptr);
	signal(SIGFPE, nullptr);
	signal(SIGILL, nullptr);
	signal(SIGABRT, nullptr);
	signal(SIGBUS, nullptr);
#endif

	disabled = true;
}

void CrashHandler::initialize() {
#ifdef CRASH_HANDLER_ENABLED
	signal(SIGSEGV, handle_crash);
	signal(SIGFPE, handle_crash);
	signal(SIGILL, handle_crash);
	signal(SIGABRT, handle_crash);
	signal(SIGBUS, handle_crash);
#endif
}
