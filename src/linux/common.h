// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2021 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#pragma once

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>

#include "../error.h"
#include "../hints.h"
#include "../resources.h"
#include "../stats.h"

#define PTHREAD_BUFFER_ITEMS 200

struct _proc_extra_info {
    unsigned int       page_size;
    char               statm_file[24];
    pthread_t          wait_thread_id;
    unsigned int       pthread_tid_offset;
    // Process creation time in clock ticks since boot (field 22 of
    // /proc/<pid>/stat).  Captured on the first liveness check and
    // compared on subsequent ones to detect PID reuse; a different value
    // means a new process now owns the PID and our target is gone.
    unsigned long long starttime;
    uintptr_t          _pthread_buffer[PTHREAD_BUFFER_ITEMS];
};

#define read_pthread_t(py_proc, addr)                                                                           \
    (copy_memory(py_proc->ref, addr, sizeof(py_proc->extra->_pthread_buffer), py_proc->extra->_pthread_buffer))

#include <sched.h>
#include <sys/wait.h>

// General ptrace wrapper. Does NOT retry on ESRCH because that error is
// terminal for most requests (thread doesn't exist or isn't traced).
static inline int
wait_ptrace(int request, pid_t pid, void* addr, void* data) {
    int outcome = ptrace(request, pid, addr, data);

    if (fail(outcome)) {
        set_error(OS, "ptrace request failed");
        FAIL;
    }

    SUCCESS;
}

// PTRACE_SEIZE wrapper that retries briefly on ESRCH. A thread that was just
// created may be momentarily invisible to ptrace while the kernel is setting up
// its task struct, so a short retry is warranted here (and only here).
static inline int
wait_ptrace_seize(pid_t pid) {
    int            outcome = 0;
    microseconds_t end     = gettime() + 100000; // 100ms

    while (gettime() < end && (outcome = ptrace(PTRACE_SEIZE, pid, 0, 0)) && errno == ESRCH)
        sched_yield();

#ifdef DEBUG
    microseconds_t wait = gettime() - end + 100000;
    if (wait > 1000)
        log_d("ptrace SEIZE long wait for pid %d: " MICROSECONDS_FMT " microseconds", pid, wait);
#endif

    if (fail(outcome)) {
        set_error(OS, "PTRACE_SEIZE failed");
        FAIL;
    }

    SUCCESS;
}

// Wait for a thread to enter ptrace-stop after PTRACE_INTERRUPT.
// PTRACE_INTERRUPT is asynchronous: it only queues the stop request; the thread
// enters ptrace-stop at the next safe point. The kernel delivers this as a
// waitpid notification, which must be consumed before any ptrace register-read
// (e.g. via libunwind _UPT_accessors) will succeed on the thread.
//
// Blocking waitpid: PTRACE_INTERRUPT is documented to interrupt blocking
// syscalls, so the stop arrives in microseconds on any working kernel and the
// kernel wakes us directly rather than us spinning on WNOHANG+sched_yield.
// The 100 ms deadline only bounds the EINTR-retry path (signal storm); with
// the default SA_RESTART behaviour of signal() it is effectively dead code.
static inline int
wait_thread_stop(pid_t tid) {
    int            status;
    microseconds_t end = gettime() + 100000;
    for (;;) {
        pid_t r = waitpid(tid, &status, __WALL);
        if (r == tid)
            return WIFSTOPPED(status) ? 0 : -1;
        if (r == -1 && errno != EINTR)
            return -1;
        if (gettime() >= end)
            return -1;
    }
}

// ----------------------------------------------------------------------------
// Side-effect-free /proc/<pid>/stat reader.  Returns false if the file cannot
// be opened (process gone) or the content is malformed.  On success, fills any
// non-NULL out parameter.  Callers care about two fields: the state code (for
// zombie/dead detection) and the starttime (a lifetime-unique value used to
// detect PID reuse).
//
// /proc/<pid>/stat format: "PID (comm) state ppid ..." with starttime at
// field 22 (1-indexed).  comm (field 2) may contain spaces and parens, so we
// scan to the last ')' to reliably locate the state field.
static inline bool
proc_stat_read(pid_t pid, char* state_out, unsigned long long* starttime_out) {
    char buffer[32];
    sprintf(buffer, "/proc/%d/stat", pid);

    cu_FILE* fp = fopen(buffer, "rb");
    if (!isvalid(fp))
        return false;

    char   line[512];
    size_t n = fread(line, 1, sizeof(line) - 1, fp);
    if (n == 0)
        return false; // cppcheck-suppress [resourceLeak]
    line[n] = '\0';

    char* p = strrchr(line, ')');
    if (!isvalid(p) || p[1] != ' ')
        return false; // cppcheck-suppress [resourceLeak]
    p += 2;           // state char

    if (state_out)
        *state_out = *p;

    if (starttime_out) {
        // p is at the state char (field 3); starttime is field 22.  Advance
        // across 18 inter-field spaces so that p + 1 lands at field 22.
        if (p[1] != ' ')
            return false; // cppcheck-suppress [resourceLeak]
        p++;              // space between field 3 and field 4
        for (int i = 0; i < 18; i++) {
            p = strchr(p + 1, ' ');
            if (!isvalid(p))
                return false; // cppcheck-suppress [resourceLeak]
        }
        *starttime_out = strtoull(p + 1, NULL, 10);
    }

    return true; // cppcheck-suppress [resourceLeak]
}

// ----------------------------------------------------------------------------
static inline FILE*
_procfs(pid_t pid, char* file) {
    FILE* fp;
    char  buffer[32];

    sprintf(buffer, "/proc/%d/%s", pid, file);

    fp = fopen(buffer, "rb");
    if (!isvalid(fp)) { // GCOV_EXCL_START
        switch (errno) {
        case EACCES: // Needs elevated privileges
            set_error(PERM, "Cannot read from procfs");
            break;
        case ENOENT: // Invalid pid
            set_error(OS, "No such process");
            break;
        default:
            set_error(OS, "Unknown error");
        } // GCOV_EXCL_STOP
    }

    return fp;
}

// ----------------------------------------------------------------------------
static inline char*
proc_root(pid_t pid, char* file) {
    if (file[0] != '/') { // GCOV_EXCL_START
        set_error(IO, "File path is not absolute");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    char* proc_root = calloc(1, strlen(file) + 24);
    if (!isvalid(proc_root)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for proc root path");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (sprintf(proc_root, "/proc/%d/root%s", pid, file) < 0) { // GCOV_EXCL_START
        free(proc_root);
        set_error(MALLOC, "Cannot format proc root path");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    return proc_root;
}

// ----------------------------------------------------------------------------
// Return an accessible path for the file backing a memory mapping.
//
// /proc/<pid>/map_files/<start>-<end> is preferred: it is a kernel-maintained
// symlink that survives file deletion and works across mount namespaces (e.g.
// containers), as long as the mapping is still alive. Falls back to
// /proc/<pid>/root/<pathname> for kernels or configurations where map_files is
// not available.
static inline char*
proc_map_file_path(pid_t pid, char* pathname, void* addr, size_t size) {
    char* path = calloc(1, 64);
    if (!isvalid(path)) // GCOV_EXCL_START
        return proc_root(pid, pathname);
    // GCOV_EXCL_STOP

    uintptr_t start = (uintptr_t)addr;
    uintptr_t upper = start + size;
    if (sprintf(path, "/proc/%d/map_files/%" PRIxPTR "-%" PRIxPTR, pid, start, upper) < 0) { // GCOV_EXCL_START
        free(path);
        return proc_root(pid, pathname);
    } // GCOV_EXCL_STOP

    struct stat s;
    if (stat(path, &s) == 0)
        return path;

    free(path);
    return proc_root(pid, pathname);
}
