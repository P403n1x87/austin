// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2026 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#include <Ntstatus.h>
#include <winternl.h>

#include "../argparse.h"
#include "../cache.h"
#include "../events.h"
#include "../logging.h"
#include "../mem.h"
#include "../py_thread.h"
#include "../stack.h"

#include "unwind.h"

// ---- Platform-specific static variables ------------------------------------

static PVOID _pi_buffer      = NULL;
static ULONG _pi_buffer_size = 0;

static hash_table_t* _handles = NULL; // tid (DWORD) -> HANDLE (thread handle)
static hash_table_t* _idle    = NULL; // tid -> (void*)1  if thread was idle before suspend
static hash_table_t* _int     = NULL; // tid -> (void*)1  if thread was suspended by us
static hash_table_t* _regs    = NULL; // tid -> thread_regs_t* (register snapshot)

typedef struct {
    uintptr_t pc;
    uintptr_t gp[GP_REG_COUNT]; // all 16 GP registers; gp[REG_RSP]=RSP, gp[REG_RBP]=RBP, etc.
} thread_regs_t;

// ---- Hot-path inline helpers: idle/interrupted state -----------------------

static inline int
_py_thread__set_idle(py_thread_t* self) {
    // Query idle state now, before the thread is suspended.
    if (py_thread__is_idle(self)) {
        hash_table__set(_idle, (key_dt)self->tid, (value_t)1);
    } else {
        hash_table__del(_idle, (key_dt)self->tid);
    }
    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__set_interrupted(py_thread_t* self, bool state) {
    if (state) {
        hash_table__set(_int, (key_dt)self->tid, (value_t)1);
    } else {
        hash_table__del(_int, (key_dt)self->tid);
    }
    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline bool
_py_thread__is_interrupted(py_thread_t* self) {
    return isvalid(hash_table__get(_int, (key_dt)self->tid));
}

// ---- Thread state query ----------------------------------------------------

// Query the OS thread state via NtQuerySystemInformation.  Used both as the
// idle-detection primitive (non-native mode) and as the pre-suspend idle
// snapshot (native mode, via py_thread__set_idle -> py_thread__is_idle).
bool
py_thread__is_idle(py_thread_t* self) {
    // In native mode the thread has already been suspended by the time the
    // sampling loop calls py_thread__is_idle.  Return the pre-suspension
    // state that was cached by py_thread__set_idle().
    if (pargs_native)
        return isvalid(hash_table__get(_idle, (key_dt)self->tid));

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

// ---- Thread control (native mode) -----------------------------------------

// Open a Win32 thread handle for the given TID and cache it in _handles.
// Idempotent: does nothing if the handle is already cached.
static inline int
_py_thread__seize(py_thread_t* self) {
    if (isvalid(hash_table__get(_handles, (key_dt)self->tid)))
        SUCCESS;

    HANDLE h
        = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, (DWORD)self->tid);
    if (h == NULL) {
        set_error(OS, "OpenThread failed");
        FAIL;
    }

    hash_table__set(_handles, (key_dt)self->tid, (value_t)h);
    log_d("win: seized thread %lu -> handle %p", (unsigned long)self->tid, h);
    SUCCESS;
}

// Suspend the thread and capture its registers into _regs.
static inline int
_py_thread__suspend(py_thread_t* self) {
    HANDLE h = (HANDLE)hash_table__get(_handles, (key_dt)self->tid);
    if (!isvalid(h)) {
        set_error(OS, "No cached handle for thread");
        FAIL;
    }

    if (SuspendThread(h) == (DWORD)-1) {
        set_error(OS, "SuspendThread failed");
        FAIL;
    }

    // Capture registers while the thread is freshly suspended.
    // Reuse the existing entry if available to avoid malloc/free per sample.
    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    if (!isvalid(regs)) {
        regs = (thread_regs_t*)malloc(sizeof(thread_regs_t));
        if (!isvalid(regs)) {
            ResumeThread(h);
            set_error(MALLOC, "Cannot allocate thread register cache");
            FAIL;
        }
        hash_table__set(_regs, (key_dt)self->tid, (value_t)regs);
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));

#if defined(_M_X64)
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(h, &ctx)) {
        ResumeThread(h);
        set_error(OS, "GetThreadContext failed during suspend");
        FAIL;
    }
    regs->pc          = (uintptr_t)ctx.Rip;
    regs->gp[REG_RAX] = (uintptr_t)ctx.Rax;
    regs->gp[REG_RCX] = (uintptr_t)ctx.Rcx;
    regs->gp[REG_RDX] = (uintptr_t)ctx.Rdx;
    regs->gp[REG_RBX] = (uintptr_t)ctx.Rbx;
    regs->gp[REG_RSP] = (uintptr_t)ctx.Rsp;
    regs->gp[REG_RBP] = (uintptr_t)ctx.Rbp;
    regs->gp[REG_RSI] = (uintptr_t)ctx.Rsi;
    regs->gp[REG_RDI] = (uintptr_t)ctx.Rdi;
    regs->gp[REG_R8]  = (uintptr_t)ctx.R8;
    regs->gp[REG_R9]  = (uintptr_t)ctx.R9;
    regs->gp[REG_R10] = (uintptr_t)ctx.R10;
    regs->gp[REG_R11] = (uintptr_t)ctx.R11;
    regs->gp[REG_R12] = (uintptr_t)ctx.R12;
    regs->gp[REG_R13] = (uintptr_t)ctx.R13;
    regs->gp[REG_R14] = (uintptr_t)ctx.R14;
    regs->gp[REG_R15] = (uintptr_t)ctx.R15;
#else
#error "Unsupported architecture for native mode on Windows"
#endif

    SUCCESS;
}

// Resume the thread.  No-op if the handle was never cached.
static inline int
_py_thread__resume(py_thread_t* self) {
    HANDLE h = (HANDLE)hash_table__get(_handles, (key_dt)self->tid);
    if (!isvalid(h))
        SUCCESS;

    // Keep the regs entry allocated for reuse in the next sample.

    if (ResumeThread(h) == (DWORD)-1) {
        set_error(OS, "ResumeThread failed");
        FAIL;
    }
    SUCCESS;
}

// Resume all interrupted threads without walking the Python thread linked list.
// Iterates _int directly (avoids N copy_remote calls).
void
py_thread__resume_all_interrupted(void) {
    key_dt interrupted[256];
    int    n = 0;
    hash_table__iteritems_start(_int, key_dt, _itid, void*, _sentinel) {
        (void)_sentinel;
        if (n < 256)
            interrupted[n++] = _itid;
    }
    hash_table__iter_stop(_int);

    for (int i = 0; i < n; i++) {
        key_dt rtid = interrupted[i];
        HANDLE h    = (HANDLE)hash_table__get(_handles, rtid);
        if (isvalid(h)) {
            if (ResumeThread(h) == (DWORD)-1) {
                log_d("win: ResumeThread failed for tid %lu", (unsigned long)rtid);
            } else {
                log_t("win: thread %lu resumed", (unsigned long)rtid);
            }
        }
        hash_table__del(_int, rtid);
    }
}

// ---- Per-thread interrupt (hot path) ----------------------------------------

int
py_thread__interrupt(py_thread_t* self) {
    if (fail(_py_thread__seize(self)))
        FAIL;

    if (fail(_py_thread__set_idle(self)))
        FAIL;

    if (fail(_py_thread__suspend(self)))
        FAIL;

    if (fail(_py_thread__set_interrupted(self, true))) {
        _py_thread__resume(self);
        FAIL;
    }

    log_t("win: thread %lu suspended", (unsigned long)self->tid);
    SUCCESS;
}

// ---- Native stack unwinding ------------------------------------------------

// Resolve a PC to a native frame (filename + function name) and push it onto
// the native stack.  Shared by both the frame-pointer walk and the .pdata
// unwind fallback paths.  Returns 0 on success, non-zero on failure.
static inline int
_push_native_frame(py_thread_t* self, uintptr_t pc) {
    lru_cache_t* cache        = self->proc->frame_cache;
    lru_cache_t* string_cache = self->proc->string_cache;

    key_dt   frame_key = (key_dt)pc;
    frame_t* frame     = lru_cache__maybe_hit(cache, frame_key);

    if (!isvalid(frame)) {
        key_dt           filename_key = (key_dt)pc;
        cached_string_t* filename     = lru_cache__maybe_hit(string_cache, filename_key);
        if (!isvalid(filename)) {
            const char* mod_path = get_module_name(self->proc->ref, pc);
            if (isvalid(mod_path)) {
                snprintf(_native_buf, MAXLEN, "%s", mod_path);
            } else {
                snprintf(_native_buf, MAXLEN, "native@%" PRIxPTR, pc);
            }
            filename = cached_string_new(filename_key, strdup(_native_buf));
            if (!isvalid(filename))
                FAIL;
            lru_cache__store(string_cache, filename_key, (value_t)filename);
            event_handler__emit_new_string(filename);
        }

        key_dt           scope_key = frame_key + 1;
        cached_string_t* scope     = lru_cache__maybe_hit(string_cache, scope_key);
        if (!isvalid(scope)) {
            const char* fname = get_func_name(self->proc->ref, pc);
            if (isvalid(fname)) {
                scope = cached_string_new(scope_key, strdup(fname));
                if (!isvalid(scope))
                    FAIL;
                lru_cache__store(string_cache, scope_key, (value_t)scope);
                event_handler__emit_new_string(scope);
            } else {
                scope = UNKNOWN_SCOPE;
            }
        }

        frame = frame_new(frame_key, filename, scope, 0, 0, 0, 0);
        if (!isvalid(frame))
            FAIL;
        lru_cache__store(cache, frame_key, (value_t)frame);
        event_handler__emit_new_frame(frame);
    }

    stack_native_push(frame);
    SUCCESS;
}

// ---- StackWalk64 single-step fallback ----------------------------------------
// Used when pdata_step cannot advance (e.g. PC outside any known module, or
// no .pdata entry for a system stub).  Performs ONE StackWalk64 step to get
// past the problematic frame, then returns control to the fast .pdata loop.
static inline bool
_stackwalk64_step(HANDLE hProcess, HANDLE hThread, uintptr_t* pc, uintptr_t gp[GP_REG_COUNT]) {
    sym_init(hProcess);

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));

    DWORD machine    = IMAGE_FILE_MACHINE_AMD64;
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip          = (DWORD64)*pc;
    ctx.Rax          = (DWORD64)gp[REG_RAX];
    ctx.Rcx          = (DWORD64)gp[REG_RCX];
    ctx.Rdx          = (DWORD64)gp[REG_RDX];
    ctx.Rbx          = (DWORD64)gp[REG_RBX];
    ctx.Rsp          = (DWORD64)gp[REG_RSP];
    ctx.Rbp          = (DWORD64)gp[REG_RBP];
    ctx.Rsi          = (DWORD64)gp[REG_RSI];
    ctx.Rdi          = (DWORD64)gp[REG_RDI];
    ctx.R8           = (DWORD64)gp[REG_R8];
    ctx.R9           = (DWORD64)gp[REG_R9];
    ctx.R10          = (DWORD64)gp[REG_R10];
    ctx.R11          = (DWORD64)gp[REG_R11];
    ctx.R12          = (DWORD64)gp[REG_R12];
    ctx.R13          = (DWORD64)gp[REG_R13];
    ctx.R14          = (DWORD64)gp[REG_R14];
    ctx.R15          = (DWORD64)gp[REG_R15];

    sf.AddrPC.Offset    = *pc;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = gp[REG_RBP];
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = gp[REG_RSP];
    sf.AddrStack.Mode   = AddrModeFlat;

    // StackWalk64's first call may return the current frame (same PC) rather
    // than stepping to the caller.  Call up to twice to ensure we advance.
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!StackWalk64(
                machine, hProcess, hThread, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL
            ))
            return false;

        uintptr_t new_pc = (uintptr_t)sf.AddrPC.Offset;
        if (new_pc == 0)
            return false;
        if (new_pc != *pc) {
            *pc         = new_pc;
            gp[REG_RSP] = (uintptr_t)sf.AddrStack.Offset;
            gp[REG_RBP] = (uintptr_t)sf.AddrFrame.Offset;
            // Sync remaining registers from context updated by StackWalk64.
            gp[REG_RAX] = (uintptr_t)ctx.Rax;
            gp[REG_RCX] = (uintptr_t)ctx.Rcx;
            gp[REG_RDX] = (uintptr_t)ctx.Rdx;
            gp[REG_RBX] = (uintptr_t)ctx.Rbx;
            gp[REG_RSI] = (uintptr_t)ctx.Rsi;
            gp[REG_RDI] = (uintptr_t)ctx.Rdi;
            gp[REG_R8]  = (uintptr_t)ctx.R8;
            gp[REG_R9]  = (uintptr_t)ctx.R9;
            gp[REG_R10] = (uintptr_t)ctx.R10;
            gp[REG_R11] = (uintptr_t)ctx.R11;
            gp[REG_R12] = (uintptr_t)ctx.R12;
            gp[REG_R13] = (uintptr_t)ctx.R13;
            gp[REG_R14] = (uintptr_t)ctx.R14;
            gp[REG_R15] = (uintptr_t)ctx.R15;
            return true;
        }
    }
    return false;
}

// ---- Native stack unwinding -------------------------------------------------
// Walk the native call stack using the fast userspace .pdata unwinder as the
// primary mechanism.  When pdata_step cannot advance (unknown module, missing
// .pdata entry, etc.), fall back to a single StackWalk64 step to get past the
// problematic frame, then resume the .pdata walk.  This gives us the speed of
// direct .pdata parsing for the majority of frames while preserving the data
// quality of StackWalk64 for edge cases (syscall stubs, JIT code, etc.).
static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    stack_native_reset();

    HANDLE hProcess = self->proc->ref;
    HANDLE hThread  = (HANDLE)hash_table__get(_handles, (key_dt)self->tid);

    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    if (!isvalid(regs)) {
        set_error(OS, "No cached register state for thread");
        FAIL;
    }

    uintptr_t pc = regs->pc;
    uintptr_t gp[GP_REG_COUNT];
    memcpy(gp, regs->gp, sizeof(gp));

    // Ensure the module table is populated.
    if (_mod_proc != hProcess || _mod_count == 0)
        modules_refresh(hProcess);

    uintptr_t prev_pc = 0;
    uintptr_t prev_sp = 0;
    while (!stack_native_full()) {
        if (pc == 0 || pc < 0x10000 || pc == prev_pc)
            break;
        prev_pc = pc;

        if (fail(_push_native_frame(self, pc)))
            FAIL;

        frame_t* top = _stack->native_base[_stack->native_pointer - 1];
        if (unlikely(is_pyeval_frame(top->scope))) {
            (void)stack_native_pop();
            stack_native_push((frame_t*)EVAL_FRAME_MAGIC);
            if (self->is_repeat)
                break;
        }

        // Save pre-step state so we can fall back to StackWalk64 if the
        // pdata unwinder produces a PC outside any known module.
        uintptr_t saved_pc = pc;
        uintptr_t saved_gp[GP_REG_COUNT];
        memcpy(saved_gp, gp, sizeof(saved_gp));

        bool stepped = pdata_step(hProcess, &pc, gp, _mod_table, _mod_count);

        // If pdata stepped but landed outside any known module, the unwind
        // likely produced wrong state.  Restore and let StackWalk64 try from
        // the original state instead.
        if (stepped && !_pc_in_module(pc, _mod_table, _mod_count)) {
            log_d("win: pdata produced out-of-module pc=%" PRIxPTR " from saved_pc=%" PRIxPTR, pc, saved_pc);
            pc = saved_pc;
            memcpy(gp, saved_gp, sizeof(gp));
            stepped = false;
        }

        // If the fast .pdata unwinder couldn't step, fall back to StackWalk64
        // for this one frame.
        if (!stepped && isvalid(hThread)) {
            stepped = _stackwalk64_step(hProcess, hThread, &pc, gp);
        }

        if (!stepped) {
            log_d(
                "win: unwind stopped at pc=%" PRIxPTR " sp=%" PRIxPTR " (pdata and stackwalk64 both failed)", saved_pc,
                saved_gp[REG_RSP]
            );
            break;
        }

        // SP must advance (grow upward) on each frame; if it doesn't, the
        // unwind produced garbage and we should stop.
        uintptr_t sp = gp[REG_RSP];
        if (sp != 0 && prev_sp != 0 && sp <= prev_sp)
            break;
        prev_sp = sp;
    }

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack */

// ---- Native unwind dispatch ------------------------------------------------

static inline void
_py_thread__unwind_native(py_thread_t* self, bool* error) {
    if (_py_thread__is_interrupted(self)) {
        if (fail(_py_thread__unwind_native_frame_stack(self))) {
            *error = true;
        }
    }
}

// ---- Allocation/deallocation -----------------------------------------------

#define WIN_THREAD_TABLE_SIZE 256

// ---- Non-Python thread sampling (native mode) ------------------------------

// Populate _pi_buffer via NtQuerySystemInformation and return a pointer to
// the SYSTEM_PROCESS_INFORMATION entry for proc->pid, or NULL on failure.
// Grows _pi_buffer as needed (same pattern as py_thread__is_idle).
static SYSTEM_PROCESS_INFORMATION*
_find_process_info(py_proc_t* proc) {
    for (;;) {
        ULONG    n;
        NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, _pi_buffer, _pi_buffer_size, &n);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            _pi_buffer_size = n;
            PVOID buf       = realloc(_pi_buffer, n);
            if (!isvalid(buf))
                return NULL;
            _pi_buffer = buf;
            continue;
        }
        if (status != STATUS_SUCCESS)
            return NULL;
        break;
    }

    SYSTEM_PROCESS_INFORMATION* pi = (SYSTEM_PROCESS_INFORMATION*)_pi_buffer;
    while (pi->UniqueProcessId != (HANDLE)proc->pid) {
        if (pi->NextEntryOffset == 0)
            return NULL;
        pi = (SYSTEM_PROCESS_INFORMATION*)(((BYTE*)pi) + pi->NextEntryOffset);
    }
    return pi;
}

// Interrupt all OS threads in the target process not already interrupted.
// Uses NtQuerySystemInformation (reuses _pi_buffer) rather than a system-wide
// Toolhelp snapshot, which only enumerates the target process's threads.
// Best-effort: failures are silently ignored.
void
py_thread__interrupt_os_threads(py_proc_t* proc) {
    SYSTEM_PROCESS_INFORMATION* pi = _find_process_info(proc);
    if (!isvalid(pi))
        return;

    SYSTEM_THREADS* ti = (SYSTEM_THREADS*)((char*)pi + sizeof(SYSTEM_PROCESS_INFORMATION));
    for (ULONG i = 0; i < pi->NumberOfThreads; i++, ti++) {
        uintptr_t tid = (uintptr_t)ti->ClientId.UniqueThread;
        if (isvalid(hash_table__get(_int, (key_dt)tid)))
            continue; // already interrupted by the Python thread sweep

        py_thread_t thread = py_thread__init(proc);
        thread.tid         = tid;
        py_thread__interrupt(&thread); // best-effort
    }
}

// Call fn(thread, userdata) for each interrupted thread that was not visited
// during the Python thread sweep.  Sets last_gen on each visited entry.
void
py_thread__for_each_non_python(
    py_proc_t* proc, thread_tracker_t* tracker, void (*fn)(py_thread_t*, void*), void* userdata
) {
    key_dt interrupted[256];
    int    n = 0;
    hash_table__iteritems_start(_int, key_dt, _itid, void*, _sentinel) {
        (void)_sentinel;
        if (n < 256)
            interrupted[n++] = _itid;
    }
    hash_table__iter_stop(_int);

    for (int i = 0; i < n; i++) {
        uintptr_t tid = (uintptr_t)interrupted[i];

        thread_tracker_entry_t* entry = thread_tracker__get_or_create(tracker, tid);
        if (!isvalid(entry))
            continue;
        if (entry->last_gen == tracker->sample_gen)
            continue; // already sampled as a Python thread

        entry->last_gen = tracker->sample_gen;

        if (!entry->name[0]) {
            HANDLE h = (HANDLE)hash_table__get(_handles, (key_dt)tid);
            if (isvalid(h)) {
                PWSTR wname = NULL;
                if (SUCCEEDED(GetThreadDescription(h, &wname)) && wname && wname[0]) {
                    WideCharToMultiByte(CP_UTF8, 0, wname, -1, entry->name, sizeof(entry->name), NULL, NULL);
                    entry->name_state = NAME_RESOLVED;
                    LocalFree(wname);
                }
            }
        }

        // Synthetic py_thread_t: only .proc and .tid are populated.
        // The unwinding and idle-check machinery needs nothing else for a
        // thread that has no PyThreadState.
        py_thread_t thread = py_thread__init(proc);
        thread.tid         = tid;
        fn(&thread, userdata);
    }
}

static int
_py_thread_allocate_native(void) {
    // On Windows we need to fetch process and thread information to detect idle
    // threads. We allocate a buffer for periodically fetching that data and, if
    // needed we grow it at runtime.
    _pi_buffer_size = (1 << 16) * sizeof(void*);
    _pi_buffer      = calloc(1, _pi_buffer_size);
    if (!isvalid(_pi_buffer)) {
        set_error(MALLOC, "Failed to allocate process information buffer");
        FAIL;
    }

    _handles = hash_table_new(WIN_THREAD_TABLE_SIZE);
    _idle    = hash_table_new(WIN_THREAD_TABLE_SIZE);
    _int     = hash_table_new(WIN_THREAD_TABLE_SIZE);
    _regs    = hash_table_new(WIN_THREAD_TABLE_SIZE);

    if (!isvalid(_handles) || !isvalid(_idle) || !isvalid(_int) || !isvalid(_regs)) {
        set_error(MALLOC, "Failed to allocate Windows thread state tables");
        hash_table__destroy(_handles);
        hash_table__destroy(_idle);
        hash_table__destroy(_int);
        hash_table__destroy(_regs);
        _handles = _idle = _int = _regs = NULL;
        FAIL;
    }
    SUCCESS;
}

static void
_py_thread_free_native(void) {
    sfree(_pi_buffer);

    // Close all cached thread handles before destroying the table.
    if (isvalid(_handles)) {
        hash_table__iter_start(_handles, void*, raw_handle) { CloseHandle((HANDLE)raw_handle); }
        hash_table__iter_stop(_handles);
    }
    hash_table__destroy(_handles);
    hash_table__destroy(_idle);
    hash_table__destroy(_int);
    if (isvalid(_regs)) {
        hash_table__iter_start(_regs, thread_regs_t*, regs) { free(regs); }
        hash_table__iter_stop(_regs);
    }
    hash_table__destroy(_regs);
    _handles = _idle = _int = _regs = NULL;
    _pdata_cache_destroy();
    sym_cleanup();
}
