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

#pragma once

#include <libproc.h>

#include "../logging.h"
#include "../py_thread.h"
#include "../resources.h"

// This offset was discovered by looking at the result of PROC_PIDLISTTHREADS.
// It's unclear whether we can rely on it always being the same, regardless of
// interpreter and OS versions.
#define SILLY_OFFSET 0xe0

#define MAX_THREADS 4096

static uint64_t _silly_offset = 0;

// ----------------------------------------------------------------------------
static void
_infer_thread_id_offset(py_thread_t* py_thread) {
    // Set the default value, in case we fail to find the actual one.
    _silly_offset = SILLY_OFFSET;

    cu_void*  tids_mem = calloc(MAX_THREADS, sizeof(uint64_t));
    uint64_t* tids     = (uint64_t*)tids_mem;
    if (!isvalid(tids)) {
        return; // cppcheck-suppress [memleak]
    }

    int n = proc_pidinfo(py_thread->proc->pid, PROC_PIDLISTTHREADS, 0, tids, MAX_THREADS * sizeof(uint64_t))
          / sizeof(uint64_t);
    if (n >= MAX_THREADS) {
        log_w("More than %d threads. Thread module initialisation might fail", MAX_THREADS);
    } else if (n <= 0) {
        log_w("No native threads found. This is weird.");
        return; // cppcheck-suppress [memleak]
    }

    // Find the thread ID offset
    uint64_t min = 0x100;
    for (int i = 0; i < n; i++) {
        uint64_t offset = tids[i] - py_thread->tid;
        if (offset < min) {
            min = offset;
        }
    }
    _silly_offset = min;
    log_t("Silly thread id offset: %x", _silly_offset);
}

// ----------------------------------------------------------------------------
bool
py_thread__is_idle(py_thread_t* self) {
    if (unlikely(_silly_offset == 0)) {
        _infer_thread_id_offset(self);
    }

    // Here we could potentially be more accurate than just reporting the
    // thread as idle, since proc_threadinfo has the pth_user_time and
    // pth_system_time fields.
    struct proc_threadinfo ti;

    if (proc_pidinfo(self->proc->pid, PROC_PIDTHREADINFO, self->tid + _silly_offset, &ti, sizeof(ti)) != sizeof(ti)) {
        set_error(OS, "Cannot get thread info");
        FAIL_BOOL;
    }

    return ti.pth_run_state != TH_STATE_RUNNING;
}
