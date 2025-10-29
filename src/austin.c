// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018 Gabriele N. Tornetta <phoenix1987@gmail.com>.
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#ifndef AUSTIN_C
#define AUSTIN_C

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "argparse.h"
#include "austin.h"
#include "env.h"
#include "error.h"
#include "events.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "mojo.h"
#include "msg.h"
#include "platform.h"
#include "python/abi.h"
#include "stats.h"
#include "timing.h"
#include "version.h"

#include "py_proc.h"
#include "py_proc_list.h"
#include "py_thread.h"

// ---- SIGNAL HANDLING -------------------------------------------------------

static int interrupt_signal = 0;

static void
signal_callback_handler(int signum) {
    log_d("Caught signal %d", signum);
    switch (signum) {
    case SIGINT:
    case SIGTERM:
        interrupt_signal = signum;
    }
} /* signal_callback_handler */

#if defined PL_WIN
BOOL WINAPI
ConsoleHandler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
        log_d("Caught Ctrl-C event");
        interrupt_signal = SIGINT;
        break;
    case CTRL_CLOSE_EVENT:
        log_d("Caught Ctrl-Close event");
        interrupt_signal = SIGTERM;
        break;
    default:
        log_d("Caught unknown console event %d", signal);
        return FALSE;
    }
    return TRUE;
}
#endif // PL_WIN

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
int
do_single_process(py_proc_t* py_proc) {
    int result = 0;

    log_meta_header();

    py_proc__log_version(py_proc, /*is_parent*/ true);

    if (pargs.exposure == 0) {
        while (interrupt_signal == 0) {
            stopwatch_start();

            if (fail(result = py_proc__sample(py_proc))) {
                // Try to re-initialise
                if (fail(py_proc__init(py_proc)))
                    FAIL_BREAK;
            }

#ifdef NATIVE
            stopwatch_pause(0);
#else
            stopwatch_pause(stopwatch_duration());
#endif
        }
    } else {
        if (!pargs.where) {
            log_m("");
            log_m("🕑 Sampling for %d second%s ...", pargs.exposure, pargs.exposure != 1 ? "s" : "");
        }
        microseconds_t end_time = gettime() + pargs.exposure * 1000000;
        while (interrupt_signal == 0) {
            stopwatch_start();

            if (fail(result = py_proc__sample(py_proc))) {
                // Try to re-initialise
                if (pargs.where || fail(py_proc__init(py_proc)))
                    FAIL_BREAK;
            }

#ifdef NATIVE
            stopwatch_pause(0);
#else
            stopwatch_pause(stopwatch_duration());
#endif
            if (pargs.where)
                break;

            if (end_time < gettime())
                interrupt_signal = SIGINT; // Emulate Ctrl-C
        }
    }

    if (pargs.attach_pid == 0) {
        if (interrupt_signal) {
            // Propagate the signal to the parent if we spawned it.
            py_proc__signal(py_proc, interrupt_signal);
        }

        // If we spawned the process, we need to wait for it to terminate.
        py_proc__terminate(py_proc);
        py_proc__wait(py_proc);
    }

    py_proc__destroy(py_proc);

    if (error_is(OS) || error_is(MEMCOPY)) {
        // When the process terminates we fail to read its memory. The OS error
        // is the signal that we no longer have a process to sample and we can
        // exit gracefully.
        SUCCESS;
    }

    return result;
} /* do_single_process */

// ----------------------------------------------------------------------------
int
do_child_processes(py_proc_t* py_proc) {
    cu_py_proc_list_t* list = py_proc_list_new(py_proc);
    if (!isvalid(list)) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    // If the parent process is not a Python process, its children might be, so
    // we attempt to attach Austin to them.
    if (!py_proc__is_python(py_proc)) {
        log_m("👽 Parent is not a Python process.");

        // Since the parent process is not running we probably have waited long
        // enough so we can try to attach to child processes straight away.
        // TODO: In the future, we might want to consider adding the option to wait
        // for child processes, as they might be spawned only much later.
        pargs.timeout = 100000; // 0.1s

        // Store the PID before it gets deleted by the update.
        pid_t ppid = py_proc->pid;

        py_proc_list__update(list);
        py_proc_list__add_proc_children(list, ppid);

        if (py_proc_list__size(list) == 1) {
            if (pargs.attach_pid == 0)
                py_proc__terminate(py_proc);
            set_error(OS, "No child processes found");
            FAIL;
        }
    } else {
        py_proc__log_version(py_proc, /*is_parent*/ true);
    }

    log_meta_header();

    if (pargs.exposure == 0) {
        while (!py_proc_list__is_empty(list) && interrupt_signal == 0) {
#ifndef NATIVE
            microseconds_t start_time = gettime();
#endif
            py_proc_list__update(list);
            py_proc_list__sample(list);
#ifdef NATIVE
            stopwatch_pause(0);
#else
            stopwatch_pause(gettime() - start_time);
#endif
        }
    } else {
        if (!pargs.pipe && !pargs.where) {
            log_m("");
            log_m("🕑 Sampling for %d second%s ...", pargs.exposure, pargs.exposure != 1 ? "s" : "");
        }
        microseconds_t end_time = gettime() + pargs.exposure * 1000000;
        while (!py_proc_list__is_empty(list) && interrupt_signal == 0) {
#ifndef NATIVE
            microseconds_t start_time = gettime();
#endif
            py_proc_list__update(list);
            py_proc_list__sample(list);
#ifdef NATIVE
            stopwatch_pause(0);
#else
            stopwatch_pause(gettime() - start_time);
#endif

            if (pargs.where)
                break;

            if (end_time < gettime())
                interrupt_signal = SIGINT; // Emulate Ctrl-C
        }
    }

    if (pargs.attach_pid == 0) {
        if (interrupt_signal) {
            // Propagate the signal to the child processes (via the parent) if
            // we spawned them.
            py_proc__signal(py_proc, interrupt_signal);
        }

        // If we spawned the child processes, we need to wait for them to
        // terminate.
        py_proc_list__update(list);
        py_proc_list__wait(list);
    }

    SUCCESS; // TODO: Fix!
} /* do_child_processes */

// ----------------------------------------------------------------------------
static inline void
handle_error() {
    log_d("Last error: %d :: %s", austin_errno, get_last_error());

    if (error_is(BINARY)) {
        _msg(MNOPYTHON);
    } else if (error_is(VERSION)) {
        _msg(MNOVERSION);
#if defined PL_UNIX
    } else if (error_is(PERM)) {
        _msg(MPERM);
#endif
    } else if (error_is(OS)) {
        _msg(pargs.attach_pid ? MATTACH : MFORK);
    } else if (error_is(MEMCOPY)) {
        // This is fine as if the process has terminated we cannot read its
        // memory.
    } else {
        _msg(MERROR); // GCOV_EXCL_LINE
    }
} /* handle_error */

int
austin() {
    int        result  = 0;
    py_proc_t* py_proc = NULL;

    if (!pargs.pipe)
        log_header(); // cppcheck-suppress [unknownMacro]

#if defined PL_MACOS
    // On MacOS, we need to be root to use Austin.
    if (geteuid() != 0) {
        set_error(PERM, "Insufficient permissions to run Austin on MacOS");
        FAIL;
    }
#endif

    if (!pargs.where && is_tty(pargs.output_file)) {
        printf(
            "\n⚠️  " BYEL "WARNING" CRESET "  Austin is about to generate binary output to terminal.\n\n"
            "Do you want to continue without specifying an output file? [y/N] "
        );
        char answer[2];
        if (fgets(answer, sizeof(answer), stdin) == NULL || !(answer[0] == 'y' || answer[0] == 'Y')) {
            SUCCESS;
        }
    }

    event_handler_t* handler = pargs.where ? where_event_handler_new() : mojo_event_handler_new();
    if (!isvalid(handler))
        FAIL; // GCOV_EXCL_LINE

    event_handler_install(handler);

    py_proc = py_proc_new(false);
    if (!isvalid(py_proc)) { // GCOV_EXCL_START
        result = 1;
        FAIL_GOTO(release);
    } // GCOV_EXCL_STOP

    if (fail(py_thread_allocate())) { // GCOV_EXCL_START
        result = 1;
        FAIL_GOTO(release);
    } // GCOV_EXCL_STOP

    // Initialise sampling metrics.
    stats_reset();

    if (pargs.attach_pid == 0) {
        if (fail(py_proc__start(py_proc, *pargs.cmd, (char**)pargs.cmd)) && !pargs.children) {
            py_proc__terminate(py_proc);
            result = 1;
            FAIL_GOTO(release);
        }
    } else {
        if (fail(py_proc__attach(py_proc, pargs.attach_pid)) && !pargs.children) {
            result = 1;
            FAIL_GOTO(release);
        }
    }

    stats_start();

    result = pargs.children ? do_child_processes(py_proc) : do_single_process(py_proc);

    // The above procedures take ownership of py_proc and are responsible for
    // destroying it. Hence once they return we need to invalidate it.
    py_proc = NULL;

    if (pargs.gc) {
        event_handler__emit_metadata("gc", MICROSECONDS_FMT, _gc_time);
    }

    if (!pargs.where)
        stats_log_metrics();

release:
    py_thread_free();
    if (isvalid(py_proc))
        py_proc__destroy(py_proc);

    log_footer();

    event_handler_free();

    return result;
} /* austin */

// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int
main(int argc, char** argv) {
    int retval = 0;

    if (fail(parse_env()))
        return austin_errno;

    if (fail(parse_args(argc, argv)))
        return austin_errno;

    logger_init();

    if (pargs.output_file != stdout)
        log_i("Output file: %s", pargs.output_filename);

    if (pargs.where) {
        log_i("Where mode on process %d", pargs.attach_pid);
        pargs.t_sampling_interval = 1;
        // We use the exposure setting to emulate sampling once
        pargs.exposure            = 1;
    } else
        log_i("Sampling interval: " MICROSECONDS_FMT " μs", pargs.t_sampling_interval);

    if (pargs.full) {
        if (pargs.memory) // GCOV_EXCL_START
            log_w("The memory switch is redundant in full mode");
        if (pargs.cpu)
            log_w("The cpu switch is redundant in full mode");
        // GCOV_EXCL_STOP
        log_i("Producing full set of metrics (time +mem -mem)");
        pargs.memory = true;
    } else if (pargs.memory) {
        if (pargs.cpu)
            log_w("The cpu switch is incompatible with memory mode.");
        pargs.cpu = false;
    }

    // Register signal handler for Ctrl+C and terminate signals.
    signal(SIGINT, signal_callback_handler);
    signal(SIGTERM, signal_callback_handler);
#if defined PL_WIN
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
#endif

    if (fail(austin())) {
        retval = 1;
        log_location();
        handle_error();
    }

    // Close the output file if it is not stdout.
    if (pargs.output_file != NULL && pargs.output_file != stdout) {
        fclose(pargs.output_file);
        log_d("Output file closed.");
    }

    logger_close();

    if (interrupt_signal)
        retval = -interrupt_signal;
    else if (fail(retval))
        retval = austin_errno;

    log_d("Exiting with code %d", retval);

    return retval;
} /* main */

#endif
