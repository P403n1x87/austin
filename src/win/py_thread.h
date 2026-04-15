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

#include <Ntstatus.h>
#include <winternl.h>

#include "../argparse.h"
#include "../py_thread.h"

static PVOID _pi_buffer      = NULL;
static ULONG _pi_buffer_size = 0;

// ----------------------------------------------------------------------------
// Query the OS thread state via NtQuerySystemInformation.  Used both as the
// idle-detection primitive (non-native mode) and as the pre-suspend idle
// snapshot (native mode, via py_thread__set_idle → py_thread__is_idle).
bool
py_thread__is_idle(py_thread_t* self) {
    // In native mode the thread has already been suspended by the time the
    // sampling loop calls py_thread__is_idle.  Return the pre-suspension
    // state that was cached by py_thread__set_idle().
    if (pargs_native)
        return isvalid(hash_table__get(_win_idle, (key_dt)self->tid));

    ULONG    n;
    NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, _pi_buffer, _pi_buffer_size, &n);
    if (status == STATUS_INFO_LENGTH_MISMATCH) {
        // Buffer was too small so we reallocate a larger one and try again.
        _pi_buffer_size   = n;
        PVOID _new_buffer = realloc(_pi_buffer, n);
        if (!isvalid(_new_buffer)) {
            set_error(MALLOC, "Cannot allocate memory for process information buffer");
            FAIL_BOOL;
        }
        _pi_buffer = _new_buffer;
        return py_thread__is_idle(self);
    }
    if (status != STATUS_SUCCESS) {
        set_error(OS, "NtQuerySystemInformation failed");
        FAIL_BOOL;
    }

    SYSTEM_PROCESS_INFORMATION* pi = (SYSTEM_PROCESS_INFORMATION*)_pi_buffer;
    while (pi->UniqueProcessId != (HANDLE)self->proc->pid) {
        if (pi->NextEntryOffset == 0) {
            // We didn't find the process, which shouldn't really happen
            set_error(OS, "Process not found");
            FAIL_BOOL;
        }
        pi = (SYSTEM_PROCESS_INFORMATION*)(((BYTE*)pi) + pi->NextEntryOffset);
    }
    log_t("[NtQuerySystemInformation] Process info found for PID %d", self->proc->pid);

    SYSTEM_THREADS* ti = (SYSTEM_THREADS*)((char*)pi + sizeof(SYSTEM_PROCESS_INFORMATION));
    for (register int i = 0; i < pi->NumberOfThreads; i++, ti++) {
        if (ti->ClientId.UniqueThread == (HANDLE)self->tid) {
            log_t("[NtQuerySystemInformation] Thread info found for TID %d", self->tid);
            return ti->State != StateRunning;
        }
    }

    set_error(OS, "Thread not found");
    FAIL_BOOL;
}

// ---- Native thread control -------------------------------------------------

// Open a Win32 thread handle for the given TID and cache it in _win_handles.
// Idempotent: does nothing if the handle is already cached.
int
_win_thread_seize(py_thread_t* self) {
    if (isvalid(hash_table__get(_win_handles, (key_dt)self->tid)))
        SUCCESS;

    HANDLE h
        = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, (DWORD)self->tid);
    if (h == NULL) {
        set_error(OS, "OpenThread failed");
        FAIL;
    }

    hash_table__set(_win_handles, (key_dt)self->tid, (value_t)h);
    log_d("win: seized thread %lu → handle %p", (unsigned long)self->tid, h);
    SUCCESS;
}

// Suspend the thread and capture its registers into _win_regs.
int
py_thread__suspend(py_thread_t* self) {
    HANDLE h = (HANDLE)hash_table__get(_win_handles, (key_dt)self->tid);
    if (!isvalid(h)) {
        set_error(OS, "No cached handle for thread");
        FAIL;
    }

    if (SuspendThread(h) == (DWORD)-1) {
        set_error(OS, "SuspendThread failed");
        FAIL;
    }

    // Capture registers while the thread is freshly suspended.
    win_thread_regs_t* regs = (win_thread_regs_t*)malloc(sizeof(win_thread_regs_t));
    if (!isvalid(regs)) {
        ResumeThread(h);
        set_error(MALLOC, "Cannot allocate thread register cache");
        FAIL;
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));

#if defined(_M_X64)
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(h, &ctx)) {
        free(regs);
        ResumeThread(h);
        set_error(OS, "GetThreadContext failed during suspend");
        FAIL;
    }
    regs->pc = (uintptr_t)ctx.Rip;
    regs->fp = (uintptr_t)ctx.Rbp;
    regs->sp = (uintptr_t)ctx.Rsp;
#elif defined(_M_ARM64)
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(h, &ctx)) {
        free(regs);
        ResumeThread(h);
        set_error(OS, "GetThreadContext failed during suspend");
        FAIL;
    }
    regs->pc = (uintptr_t)ctx.Pc;
    regs->fp = (uintptr_t)ctx.Fp;
    regs->sp = (uintptr_t)ctx.Sp;
#else
#error "Unsupported architecture for native mode on Windows"
#endif

    // Free any stale entry from a prior sample.
    win_thread_regs_t* prev = (win_thread_regs_t*)hash_table__get(_win_regs, (key_dt)self->tid);
    sfree(prev);
    hash_table__set(_win_regs, (key_dt)self->tid, (value_t)regs);

    SUCCESS;
}

// Resume the thread.  No-op if the handle was never cached.
int
py_thread__resume(py_thread_t* self) {
    HANDLE h = (HANDLE)hash_table__get(_win_handles, (key_dt)self->tid);
    if (!isvalid(h))
        SUCCESS;

    win_thread_regs_t* regs = (win_thread_regs_t*)hash_table__get(_win_regs, (key_dt)self->tid);
    sfree(regs);
    hash_table__del(_win_regs, (key_dt)self->tid);

    if (ResumeThread(h) == (DWORD)-1) {
        set_error(OS, "ResumeThread failed");
        FAIL;
    }
    SUCCESS;
}

// Resume all interrupted threads without walking the Python thread linked list.
// Iterates _win_int directly (avoids N copy_remote calls).
void
py_thread__resume_all_interrupted(void) {
    key_dt interrupted[256];
    int    n = 0;
    hash_table__iteritems_start(_win_int, key_dt, _itid, void*, _sentinel) {
        (void)_sentinel;
        if (n < 256)
            interrupted[n++] = _itid;
    }
    hash_table__iter_stop(_win_int);

    for (int i = 0; i < n; i++) {
        key_dt rtid = interrupted[i];
        HANDLE h    = (HANDLE)hash_table__get(_win_handles, rtid);
        if (isvalid(h)) {
            if (ResumeThread(h) == (DWORD)-1) {
                log_d("win: ResumeThread failed for tid %lu", (unsigned long)rtid);
            } else {
                log_t("win: thread %lu resumed", (unsigned long)rtid);
            }
        }
        win_thread_regs_t* regs = (win_thread_regs_t*)hash_table__get(_win_regs, rtid);
        sfree(regs);
        hash_table__del(_win_regs, rtid);
        hash_table__del(_win_int, rtid);
    }
}
