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
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/time.h>

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
//
// PTRACE_O_TRACEEXEC: without it, a successful execve(2) by the tracee still
// stops it (the kernel does this unconditionally), but as a *plain*
// signal-delivery-stop indistinguishable from a real, unrelated SIGTRAP --
// see wait_thread_stop's own doc comment for why that ambiguity matters.
// With it set, the same stop instead carries PTRACE_EVENT_EXEC in the status
// word's event field, so it can be told apart from everything else without
// guessing.
static inline int
wait_ptrace_seize(pid_t pid) {
    int            outcome = 0;
    microseconds_t end     = gettime() + 100000; // 100ms

    while (gettime() < end && (outcome = ptrace(PTRACE_SEIZE, pid, 0, PTRACE_O_TRACEEXEC)) && errno == ESRCH)
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

// One-shot SIGALRM used solely to put a hard ceiling on the blocking waitpid
// in wait_thread_stop below -- see that function's own doc comment for why a
// deadline that's only checked between waitpid returns isn't enough on its
// own. The handler does nothing; EINTR is all that's needed to unblock.
static void
_wait_thread_stop_alarm(int sig) {
    (void)sig;
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
//
// Hard deadline via SIGALRM: the 100 ms deadline below is only ever checked
// *between* waitpid calls, which isn't enough by itself -- a specific thread
// (identified by this exact tid) can be torn down as part of whole-process
// exit without the kernel ever delivering a wait-visible status for that one
// tid (only a process-wide/wildcard wait is guaranteed to see it), so a
// blocking, single-tid waitpid can have nothing left to ever wake it. This
// is indistinguishable up front from a thread that just hasn't stopped yet,
// so an independent timeout that fires regardless of what waitpid does is
// the only way to bound this call. The alarm is purely to generate EINTR;
// the actual deadline enforcement is still the gettime() check below.
//
// A ptrace-stop can also arrive for a reason that has nothing to do with our
// own PTRACE_INTERRUPT: an exec-in-place target can stop the thread with a
// kernel-generated exec notification independently of our sampling cycle
// (wait_ptrace_seize sets PTRACE_O_TRACEEXEC so this one is unambiguous --
// see its own doc comment), or, in principle, a genuine unrelated signal
// could. Mistaking either for the response to *our* PTRACE_INTERRUPT (or
// vice versa) would permanently desync the two: whichever goes
// unacknowledged leaves the thread parked in ptrace-stop forever, waiting
// for a PTRACE_CONT that nothing will ever issue.
//
// Empirically confirmed (see the three stop kinds a tracee can report,
// distinguished by the event field at status>>16 -- WSTOPSIG(status) alone
// is not enough, since it reports SIGTRAP for both our own stop and an exec
// notification):
//   our own PTRACE_INTERRUPT-stop: WSTOPSIG==SIGTRAP, event==PTRACE_EVENT_STOP
//   exec notification (PTRACE_O_TRACEEXEC): WSTOPSIG==SIGTRAP, event==PTRACE_EVENT_EXEC
//   a genuine unrelated signal:              WSTOPSIG==<that signal>, event==0
// Only the first is ours to consume here; anything else is acknowledged
// (forwarding the real signal if there is one -- an exec notification has
// none to forward) and the wait resumes.
//
// exec_out: when non-NULL, set to true if an exec notification was observed
// along the way. The caller uses this to tell its own process-level state
// (istate_raddr, bin_path, cached symbols, ...) to be re-derived from
// scratch: an in-place exec gives the new image a fresh ASLR base, so none
// of it is valid anymore, even though a stale thread-state head can easily
// still look like a plausible (non-NULL) pointer rather than failing
// outright -- relying on sampling to notice the data is wrong is not
// reliable, but the kernel's own exec notification is.
static inline int
wait_thread_stop(pid_t tid, bool* exec_out) {
    if (exec_out)
        *exec_out = false;

    struct sigaction sa = {0}, old_sa;
    sa.sa_handler       = _wait_thread_stop_alarm;
    sigemptyset(&sa.sa_mask); // No SA_RESTART: EINTR must actually interrupt waitpid.
    sigaction(SIGALRM, &sa, &old_sa);

    struct itimerval timer = {0}, old_timer;
    timer.it_value.tv_usec = 100000; // 100ms, matches the deadline below
    setitimer(ITIMER_REAL, &timer, &old_timer);

    microseconds_t end    = gettime() + 100000;
    int            result = -1;
    for (;;) {
        int   status;
        pid_t r = waitpid(tid, &status, __WALL);
        if (r == tid) {
            if (!WIFSTOPPED(status))
                break;
            if ((status >> 16) == PTRACE_EVENT_STOP) {
                result = 0; // Our own group-stop; thread is now fully stopped.
                break;
            }

            if ((status >> 16) == PTRACE_EVENT_EXEC && exec_out)
                *exec_out = true;

            int sig = (status >> 16) == 0 ? WSTOPSIG(status) : 0;
            ptrace(PTRACE_CONT, tid, 0, (void*)(intptr_t)sig);
            if (gettime() >= end)
                break;
            continue;
        }
        if (r == -1 && errno != EINTR)
            break;
        if (gettime() >= end)
            break;
    }

    setitimer(ITIMER_REAL, &old_timer, NULL);
    sigaction(SIGALRM, &old_sa, NULL);

    return result;
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
// /proc/<pid>/root/<pathname> is tried first — it handles the normal case and
// real containers (different mount namespaces) transparently.  If that path is
// inaccessible (e.g. the file has been deleted, as in a container-like
// scenario), we fall back to /proc/<pid>/map_files/<start>-<end>, a
// kernel-maintained symlink that stays valid via the inode reference kept by
// the mapping even after the file is unlinked.
static inline char*
proc_map_file_path(pid_t pid, char* pathname, void* addr, size_t size) {
    char* root_path = proc_root(pid, pathname);
    if (isvalid(root_path)) {
        struct stat s;
        if (stat(root_path, &s) == 0)
            return root_path;
        free(root_path);
    }

    // proc_root path inaccessible (file deleted) — fall back to map_files
    char* path = calloc(1, 64);
    if (!isvalid(path)) // GCOV_EXCL_START
        return NULL;
    // GCOV_EXCL_STOP

    uintptr_t start = (uintptr_t)addr;
    uintptr_t upper = start + size;
    if (sprintf(path, "/proc/%d/map_files/%" PRIxPTR "-%" PRIxPTR, pid, start, upper) < 0) { // GCOV_EXCL_START
        free(path);
        return NULL;
    } // GCOV_EXCL_STOP

    return path;
}
