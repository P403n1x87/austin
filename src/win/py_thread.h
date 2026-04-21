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

#include <Ntstatus.h>
#include <winternl.h>

#include "../argparse.h"
#include "../cache.h"
#include "../events.h"
#include "../logging.h"
#include "../mem.h"
#include "../py_thread.h"
#include "../stack.h"

// ---- Platform-specific static variables ------------------------------------

static PVOID _pi_buffer      = NULL;
static ULONG _pi_buffer_size = 0;

static hash_table_t* _handles = NULL; // tid (DWORD) -> HANDLE (thread handle)
static hash_table_t* _idle    = NULL; // tid -> (void*)1  if thread was idle before suspend
static hash_table_t* _int     = NULL; // tid -> (void*)1  if thread was suspended by us
static hash_table_t* _regs    = NULL; // tid -> thread_regs_t* (PC/FP/SP captured at suspend)

typedef struct {
    uintptr_t pc;
    uintptr_t fp;
    uintptr_t sp;
#if defined(_M_ARM64)
    uintptr_t lr;    // x30 — holds the return address for leaf functions
    CONTEXT   saved; // full CONTEXT — StackWalk64 needs X19..X28 to replay pdata unwind ops
#endif
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
    thread_regs_t* regs = (thread_regs_t*)malloc(sizeof(thread_regs_t));
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
    ctx.ContextFlags = CONTEXT_FULL; // need X19..X28 for StackWalk64 unwind-op replay
    if (!GetThreadContext(h, &ctx)) {
        free(regs);
        ResumeThread(h);
        set_error(OS, "GetThreadContext failed during suspend");
        FAIL;
    }
    regs->pc    = (uintptr_t)ctx.Pc;
    regs->fp    = (uintptr_t)ctx.Fp;
    regs->sp    = (uintptr_t)ctx.Sp;
    regs->lr    = (uintptr_t)ctx.Lr;
    regs->saved = ctx;
#else
#error "Unsupported architecture for native mode on Windows"
#endif

    // Free any stale entry from a prior sample.
    thread_regs_t* prev = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    sfree(prev);
    hash_table__set(_regs, (key_dt)self->tid, (value_t)regs);

    SUCCESS;
}

// Resume the thread.  No-op if the handle was never cached.
static inline int
_py_thread__resume(py_thread_t* self) {
    HANDLE h = (HANDLE)hash_table__get(_handles, (key_dt)self->tid);
    if (!isvalid(h))
        SUCCESS;

    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    sfree(regs);
    hash_table__del(_regs, (key_dt)self->tid);

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
        thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, rtid);
        sfree(regs);
        hash_table__del(_regs, rtid);
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
// the native stack.  Shared by both the frame-pointer walk and the StackWalk64
// fallback paths.  Returns 0 on success, non-zero on failure.
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

// ---- StackWalk64 fallback ---------------------------------------------------
// Used when the frame-pointer walk fails to produce a deep stack (typically
// because the binary was compiled with frame-pointer omission, which is the
// MSVC x64 default).  StackWalk64 uses the PE .pdata unwind info and is
// slower but handles all calling conventions correctly.
static inline int
_unwind_stackwalk64(py_thread_t* self) {
    stack_native_reset();

    HANDLE hProcess = self->proc->ref;
    HANDLE hThread  = (HANDLE)hash_table__get(_handles, (key_dt)self->tid);
    if (!isvalid(hThread)) {
        set_error(OS, "No cached handle for thread");
        FAIL;
    }

    sym_init(hProcess);
    // Ensure the module table is populated before the walk — we use it below
    // to validate return addresses emitted by StackWalk64.
    if (_mod_proc != hProcess || _mod_count == 0)
        modules_refresh(hProcess);

    // Build a fresh CONTEXT for StackWalk64 from the cached registers.
    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    if (!isvalid(regs)) {
        set_error(OS, "No cached register state for thread");
        FAIL;
    }

    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
#if defined(_M_X64)
    DWORD machine    = IMAGE_FILE_MACHINE_AMD64;
    ctx.ContextFlags = CONTEXT_FULL;
    ctx.Rip          = (DWORD64)regs->pc;
    ctx.Rbp          = (DWORD64)regs->fp;
    ctx.Rsp          = (DWORD64)regs->sp;
#elif defined(_M_ARM64)
    DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    // Reuse the full CONTEXT captured at suspend time.  StackWalk64 on ARM64
    // relies on X19..X28 to replay pdata unwind ops; rebuilding the CONTEXT
    // from scratch would zero those and yield garbage return addresses.
    ctx           = regs->saved;
#endif

    STACKFRAME64 sf;
    memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset    = regs->pc;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = regs->fp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = regs->sp;
    sf.AddrStack.Mode   = AddrModeFlat;

    uintptr_t prev_pc = 0;
    uintptr_t prev_sp = 0;
    while (!stack_native_full()) {
        if (!StackWalk64(
                machine, hProcess, hThread, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL
            ))
            break;

        uintptr_t pc = (uintptr_t)sf.AddrPC.Offset;
        if (pc == 0)
            break;

        // Cycle-guard: StackWalk64 on ARM64 can stall on a bogus return address
        // and keep re-emitting it (or walk a garbage loop of ascending PCs).
        // Break if the PC repeats or SP fails to strictly advance — both are
        // unambiguous signals that the unwind is no longer making progress.
        if (pc == prev_pc)
            break;
        uintptr_t sp = (uintptr_t)sf.AddrStack.Offset;
        if (prev_sp != 0 && sp != 0 && sp <= prev_sp)
            break;
        prev_pc = pc;
        prev_sp = sp;

        // Bogus-PC guard: StackWalk64 on ARM64 frequently emits a garbage
        // return address as its final frame (a non-canonical address outside
        // any loaded module).  Reject those instead of pushing them as
        // native@... frames that pollute the output — the walk is done.
        // Use get_module_name (not plain _mod_lookup) so a miss triggers a
        // module-table refresh, covering DLLs loaded after the initial enum.
        if (get_module_name(hProcess, pc) == NULL)
            break;

        if (fail(_push_native_frame(self, pc)))
            FAIL;
    }

    SUCCESS;
}

// Walk the native call stack of a suspended thread.
//
// Strategy: try the fast frame-pointer chain first.  If that produces at most
// one frame (RBP wasn't a real frame pointer — typical for MSVC /Oy binaries),
// fall back to StackWalk64 which uses PE .pdata unwind info and handles all
// calling conventions at the cost of extra API calls per frame.
static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    stack_native_reset();

    // ---- Seed registers from the state captured at suspend time ----
    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    if (!isvalid(regs)) {
        set_error(OS, "No cached register state for thread");
        FAIL;
    }
    uintptr_t pc = regs->pc;
    uintptr_t fp = regs->fp;
    uintptr_t sp = regs->sp;

    // Quick sanity check: if fp looks like a plausible stack address (between
    // sp and sp + 1MB) attempt the frame-pointer walk.  Otherwise skip straight
    // to StackWalk64.  On ARM64 the compiler routinely omits x29 (frame
    // pointer) saves, so the fp-walk almost never produces useful stacks —
    // skip it entirely and rely on StackWalk64's PE .pdata unwinder.
#if defined(_M_ARM64)
    bool try_fp_walk = false;
#else
    bool try_fp_walk = (fp > sp && fp < sp + (1 << 20));
#endif

    if (try_fp_walk) {
        // ---- Prefetch one page of stack into a local buffer ----
#define _STACK_BUF_SIZE 4096
        uint8_t   _stack_buf[_STACK_BUF_SIZE];
        uintptr_t _stack_buf_base = sp & ~((uintptr_t)(_STACK_BUF_SIZE - 1));
        {
            SIZE_T _sz = 0;
            if (!ReadProcessMemory(self->proc->ref, (LPCVOID)_stack_buf_base, _stack_buf, _STACK_BUF_SIZE, &_sz)
                || _sz != _STACK_BUF_SIZE) {
                _stack_buf_base = 0;
            }
        }

        // ---- Walk frame-pointer chain ----
        while (!stack_native_full() && pc != 0) {
            if (fail(_push_native_frame(self, pc)))
                FAIL;

            if (fp == 0)
                break;

            uintptr_t frame_data[2] = {0, 0};
            if (_stack_buf_base != 0 && fp >= _stack_buf_base
                && fp - _stack_buf_base + sizeof(frame_data) <= _STACK_BUF_SIZE) {
                memcpy(frame_data, _stack_buf + (fp - _stack_buf_base), sizeof(frame_data));
            } else {
                uintptr_t new_base = fp & ~((uintptr_t)(_STACK_BUF_SIZE - 1));
                SIZE_T    _sz      = 0;
                if (ReadProcessMemory(self->proc->ref, (LPCVOID)new_base, _stack_buf, _STACK_BUF_SIZE, &_sz)
                    && _sz == _STACK_BUF_SIZE) {
                    _stack_buf_base = new_base;
                    memcpy(frame_data, _stack_buf + (fp - _stack_buf_base), sizeof(frame_data));
                } else {
                    _stack_buf_base  = 0;
                    SIZE_T read_size = 0;
                    if (!ReadProcessMemory(self->proc->ref, (LPCVOID)fp, frame_data, sizeof(frame_data), &read_size)
                        || read_size != sizeof(frame_data)) {
                        break;
                    }
                }
            }

            fp = frame_data[0];
            pc = frame_data[1];
        }
#undef _STACK_BUF_SIZE

        // If fp-walk produced a deep stack, we're done.
        if (_stack->native_pointer > 1)
            SUCCESS;

        // fp-walk was flat — fall through to StackWalk64.
        log_d("win: fp-walk produced %zd frame(s), falling back to StackWalk64", _stack->native_pointer);
    }

    // ---- StackWalk64 fallback ----
    return _unwind_stackwalk64(self);
} /* _py_thread__unwind_native_frame_stack */

// ---- Native unwind dispatch ------------------------------------------------

static inline void
_py_thread__unwind_native(py_thread_t* self, bool* error) {
    if (pargs_native && _py_thread__is_interrupted(self)) {
        if (fail(_py_thread__unwind_native_frame_stack(self))) {
            *error = true;
        }
    }
}

// ---- Allocation/deallocation -----------------------------------------------

#define WIN_THREAD_TABLE_SIZE 256

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
    sym_cleanup();
}
