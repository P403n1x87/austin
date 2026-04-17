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
#include <mach/mach.h>
#include <sys/sysctl.h>

#include "../argparse.h"
#include "../cache.h"
#include "../events.h"
#include "../logging.h"
#include "../mem.h"
#include "../py_thread.h"
#include "../resources.h"
#include "../stack.h"

#include "dyld_cache.h"

// ---- Platform-specific static variables ------------------------------------

static hash_table_t* _ports = NULL; // pthread_t -> thread_act_t (Mach thread port)
static hash_table_t* _idle  = NULL; // pthread_t -> (void*)1  if thread was idle before suspend
static hash_table_t* _int   = NULL; // pthread_t -> (void*)1  if thread was suspended by us
static hash_table_t* _regs  = NULL; // pthread_t -> thread_regs_t* (PC/FP/SP captured at suspend)

typedef struct {
    uintptr_t pc;
    uintptr_t fp;
    uintptr_t sp;
} thread_regs_t;

// ---- Thread state query ----------------------------------------------------
// Defined before the hot-path helpers because _py_thread__set_idle calls
// _py_thread__is_idle_now.

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
// Raw idle check.  In NATIVE mode, called after the port has been seized so
// we can use thread_info(THREAD_BASIC_INFO) directly — faster than going
// through the proc_info layer, and avoids the _silly_offset dependency.
// In non-NATIVE mode, falls back to proc_pidinfo (no port available yet).
static inline bool
_py_thread__is_idle_now(py_thread_t* self) {
    if (pargs_native) {
        thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_ports, (key_dt)self->tid);
        if (port) {
            thread_basic_info_data_t info  = {0};
            mach_msg_type_number_t   count = THREAD_BASIC_INFO_COUNT;
            if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS)
                return info.run_state != TH_STATE_RUNNING;
        }
        // Port not yet cached (shouldn't happen if seize runs first) — fall through.
    }
    if (unlikely(_silly_offset == 0))
        _infer_thread_id_offset(self);

    struct proc_threadinfo ti;
    if (proc_pidinfo(self->proc->pid, PROC_PIDTHREADINFO, self->tid + _silly_offset, &ti, sizeof(ti)) != sizeof(ti)) {
        set_error(OS, "Cannot get thread info");
        FAIL_BOOL;
    }
    return ti.pth_run_state != TH_STATE_RUNNING;
}

// ----------------------------------------------------------------------------
bool
py_thread__is_idle(py_thread_t* self) {
    // In native mode the thread has already been suspended by the time this is
    // called from the sampling loop.  Return the pre-suspension state that was
    // cached by py_thread__set_idle().
    if (pargs_native)
        return isvalid(hash_table__get(_idle, (key_dt)self->tid));
    return _py_thread__is_idle_now(self);
}

// ---- Hot-path inline helpers: idle/interrupted state -----------------------

static inline int
_py_thread__set_idle(py_thread_t* self) {
    // Query idle state now, before the thread is suspended.
    if (_py_thread__is_idle_now(self)) {
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

// ---- Thread control (native mode) -----------------------------------------

// Find the Mach thread port for self->tid (a pthread_t value) and cache it
// in _ports.  Idempotent: does nothing if the port is already cached.
static inline int
_py_thread__seize(py_thread_t* self) {
    if (isvalid(hash_table__get(_ports, (key_dt)self->tid)))
        SUCCESS;

    // _silly_offset adjusts pthread_t to the value stored in thread_handle.
    // It is initialised by _py_thread__is_idle_now(), called before us in
    // _py_proc__interrupt_threads.  Fall back to SILLY_OFFSET if not yet set.
    if (unlikely(_silly_offset == 0))
        _infer_thread_id_offset(self);

    thread_act_t port = _find_thread_port(self->proc->ref, self->tid + _silly_offset);
    if (port == MACH_PORT_NULL) {
        set_error(OS, "Failed to find Mach thread port");
        FAIL;
    }

    hash_table__set(_ports, (key_dt)self->tid, (value_t)(uintptr_t)port);
    log_d("mac: seized thread %p -> port %u", (void*)self->tid, port);
    SUCCESS;
}

// Suspend the thread and capture its registers into _regs.
// Locates and caches the Mach port if not already done.
static inline int
_py_thread__suspend(py_thread_t* self) {
    if (fail(_py_thread__seize(self)))
        FAIL;

    thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_ports, (key_dt)self->tid);

    // NOTE: profiling shows thread_suspend is the dominant cost in native-mode
    // sampling (~23% of py_proc__sample).  thread_get_state works on live
    // threads too, so we could skip the suspend/resume pair and walk the stack
    // opportunistically at the cost of occasional torn frames.  We keep the
    // suspend for now because native code is generally much faster than CPython
    // and its stacks mutate more rapidly, so the window for a corrupt read is
    // larger here than it is for pure Python sampling.
    if (thread_suspend(port) != KERN_SUCCESS) {
        set_error(OS, "thread_suspend failed");
        FAIL;
    }

    // Capture PC/FP/SP while the thread is freshly suspended so that
    // _py_thread__unwind_native_frame_stack can use them directly, avoiding
    // a second thread_get_state Mach call per thread per sample.
    thread_regs_t* regs = (thread_regs_t*)malloc(sizeof(thread_regs_t));
    if (!isvalid(regs)) {
        thread_resume(port);
        set_error(MALLOC, "Cannot allocate thread register cache");
        FAIL;
    }

#if defined(__x86_64__)
    x86_thread_state64_t   state = {0};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(port, x86_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
        free(regs);
        thread_resume(port);
        set_error(OS, "thread_get_state failed during suspend");
        FAIL;
    }
    regs->pc = (uintptr_t)state.__rip;
    regs->fp = (uintptr_t)state.__rbp;
    regs->sp = (uintptr_t)state.__rsp;
#elif defined(__arm64__)
    arm_thread_state64_t   state = {0};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
        free(regs);
        thread_resume(port);
        set_error(OS, "thread_get_state failed during suspend");
        FAIL;
    }
    regs->pc = (uintptr_t)arm_thread_state64_get_pc(state);
    regs->fp = (uintptr_t)arm_thread_state64_get_fp(state);
    regs->sp = (uintptr_t)arm_thread_state64_get_sp(state);
#endif

    // Free any stale entry from a prior sample (e.g. error-path resume).
    thread_regs_t* prev = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    sfree(prev);
    hash_table__set(_regs, (key_dt)self->tid, (value_t)regs);

    SUCCESS;
}

// Resume the thread.  No-op if the port was never cached.
static inline int
_py_thread__resume(py_thread_t* self) {
    thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_ports, (key_dt)self->tid);
    if (!port)
        SUCCESS;

    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    sfree(regs);
    hash_table__del(_regs, (key_dt)self->tid);

    if (thread_resume(port) != KERN_SUCCESS) {
        set_error(OS, "thread_resume failed");
        FAIL;
    }
    SUCCESS;
}

// Resume all interrupted threads without walking the Python thread linked list.
// Iterates _int directly (avoids N copy_remote calls).
// The hash_table iterator is not mutation-safe, so collect TIDs first.
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
        key_dt       rtid = interrupted[i];
        thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_ports, rtid);
        if (port != MACH_PORT_NULL) {
            if (thread_resume(port) != KERN_SUCCESS) {
                log_d("mac: thread_resume failed for port %u", port);
            } else {
                log_t("mac: thread %p resumed", (void*)rtid);
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

    log_t("mac: thread %p suspended", (void*)self->tid);
    SUCCESS;
}

// ---- Native stack unwinding ------------------------------------------------

// Mask off pointer-authentication bits from a return address on arm64.
// User-space VAs on Apple Silicon are at most 39 bits wide.
#if defined(__arm64__)
#define _STRIP_PAC(addr) ((uintptr_t)(addr) & 0x0000007fffffffffull)
#else
#define _STRIP_PAC(addr) ((uintptr_t)(addr))
#endif

// Walk the native call stack of a suspended thread using the frame-pointer
// chain.  Initial registers are obtained via thread_get_state(); subsequent
// frames are read from the remote address space with mach_vm_read_overwrite().
//
// The filename is set to the path of the mapped binary obtained via
// proc_regionfilename(), or "native@<pc>" when the mapping is unknown.
// Scope (function name) is resolved via get_func_name() which reads
// the Mach-O LC_SYMTAB and performs an ASLR-adjusted binary search.
static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    stack_native_reset();

    // ---- Seed registers from the state captured at suspend time ----
    // _py_thread__suspend() already called thread_get_state and cached the
    // result in _regs, so we avoid a redundant Mach trap here.
    thread_regs_t* regs = (thread_regs_t*)hash_table__get(_regs, (key_dt)self->tid);
    if (!isvalid(regs)) {
        set_error(OS, "No cached register state for thread");
        FAIL;
    }
    uintptr_t pc = regs->pc;
    uintptr_t fp = regs->fp;
    uintptr_t sp = regs->sp;

    lru_cache_t* cache        = self->proc->frame_cache;
    lru_cache_t* string_cache = self->proc->string_cache;

    // ---- Prefetch one page of stack into a local buffer -------------------
    // Replaces per-frame mach_vm_read_overwrite(16 bytes) with a single read
    // for frames whose FP falls within the page.  Falls back to per-frame
    // reads for deeper/split stacks.
#define _STACK_BUF_SIZE 4096
    uint8_t   _stack_buf[_STACK_BUF_SIZE];
    uintptr_t _stack_buf_base = sp & ~((uintptr_t)(_STACK_BUF_SIZE - 1));
    {
        mach_vm_size_t _sz = 0;
        if (mach_vm_read_overwrite(
                self->proc->ref, (mach_vm_address_t)_stack_buf_base, _STACK_BUF_SIZE, (mach_vm_address_t)_stack_buf,
                &_sz
            ) != KERN_SUCCESS
            || _sz != _STACK_BUF_SIZE) {
            _stack_buf_base = 0; // disable buffer; use per-frame fallback
        }
    }

    // ---- Walk frame-pointer chain ----
    while (!stack_native_full() && pc != 0) {
        key_dt   frame_key = (key_dt)pc;
        frame_t* frame     = lru_cache__maybe_hit(cache, frame_key);

        if (!isvalid(frame)) {
            // Resolve filename from the mapped binary that owns this PC.
            char region_path[MAXPATHLEN + 1] = {0};
            int  path_len                    = -1;

            key_dt           filename_key = (key_dt)pc;
            cached_string_t* filename     = lru_cache__maybe_hit(string_cache, filename_key);
            if (!isvalid(filename)) {
                // Fast path: if the PC is in the dyld shared cache range,
                // resolve the path directly from the cache — proc_regionfilename
                // always fails for shared-cache addresses and is a wasted Mach trap.
                if (_dsc_pc_in_cache(self->proc->ref, pc)) {
                    const char* cache_path = _dsc_path_for_pc(self->proc->ref, pc, NULL);
                    if (cache_path) {
                        strncpy(region_path, cache_path, MAXPATHLEN);
                        region_path[MAXPATHLEN] = '\0';
                        path_len                = (int)strlen(cache_path);
                    }
                } else {
                    path_len = proc_regionfilename(self->proc->pid, pc, region_path, MAXPATHLEN);
                }
                if (path_len > 0) {
                    snprintf(_native_buf, MAXLEN, "%s", region_path);
                } else {
                    snprintf(_native_buf, MAXLEN, "native@%" PRIxPTR, pc);
                }
                filename = cached_string_new(filename_key, strdup(_native_buf));
                if (!isvalid(filename))
                    FAIL;
                lru_cache__store(string_cache, filename_key, (value_t)filename);
                event_handler__emit_new_string(filename);
            }

            // Resolve scope (function name) via Mach-O symbol table.
            key_dt           scope_key = frame_key + 1;
            cached_string_t* scope     = lru_cache__maybe_hit(string_cache, scope_key);
            if (!isvalid(scope)) {
                // path_len is -1 when filename was already cached; re-resolve.
                if (path_len < 0) {
                    if (_dsc_pc_in_cache(self->proc->ref, pc)) {
                        const char* cache_path = _dsc_path_for_pc(self->proc->ref, pc, NULL);
                        if (cache_path) {
                            strncpy(region_path, cache_path, MAXPATHLEN);
                            region_path[MAXPATHLEN] = '\0';
                            path_len                = (int)strlen(cache_path);
                        }
                    } else {
                        path_len = proc_regionfilename(self->proc->pid, pc, region_path, MAXPATHLEN);
                    }
                }
                const char* fname = NULL;
                if (path_len > 0)
                    fname = get_func_name(self->proc->ref, self->proc->pid, pc, region_path);
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

        if (fp == 0)
            break;

        // Read the next frame record: [fp] = saved_fp, [fp+8] = return address.
        // Serve from the prefetched stack buffer when possible; otherwise re-read
        // a new page (covers frames in deeper stack regions or split across pages).
        uintptr_t frame_data[2] = {0, 0};
        if (_stack_buf_base != 0 && fp >= _stack_buf_base
            && fp - _stack_buf_base + sizeof(frame_data) <= _STACK_BUF_SIZE) {
            memcpy(frame_data, _stack_buf + (fp - _stack_buf_base), sizeof(frame_data));
        } else {
            // fp is outside the current buffer: read the page that contains fp.
            uintptr_t      new_base = fp & ~((uintptr_t)(_STACK_BUF_SIZE - 1));
            mach_vm_size_t _sz      = 0;
            if (mach_vm_read_overwrite(
                    self->proc->ref, (mach_vm_address_t)new_base, _STACK_BUF_SIZE, (mach_vm_address_t)_stack_buf, &_sz
                ) == KERN_SUCCESS
                && _sz == _STACK_BUF_SIZE) {
                _stack_buf_base = new_base;
                memcpy(frame_data, _stack_buf + (fp - _stack_buf_base), sizeof(frame_data));
            } else {
                // Give up on buffering; single-frame fallback.
                _stack_buf_base          = 0;
                mach_vm_size_t read_size = 0;
                if (mach_vm_read_overwrite(
                        self->proc->ref, (mach_vm_address_t)fp, sizeof(frame_data), (mach_vm_address_t)frame_data,
                        &read_size
                    ) != KERN_SUCCESS
                    || read_size != sizeof(frame_data)) {
                    break;
                }
            }
        }

        fp = frame_data[0];
        pc = _STRIP_PAC(frame_data[1]);
    }
#undef _STACK_BUF_SIZE

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack */

// ---- Native unwind dispatch ------------------------------------------------

static inline void
_py_thread__unwind_native(py_thread_t* self, bool* error) {
    if (pargs_native && fail(_py_thread__unwind_native_frame_stack(self))) {
        *error = true;
    }
    // No re-read here: the thread is suspended (thread_suspend) so its
    // state cannot change between _py_proc__interrupt_threads and now.
}

// ---- Allocation/deallocation -----------------------------------------------

#define MAC_THREAD_TABLE_SIZE 256

static int
_py_thread_allocate_native(void) {
    _ports = hash_table_new(MAC_THREAD_TABLE_SIZE);
    _idle  = hash_table_new(MAC_THREAD_TABLE_SIZE);
    _int   = hash_table_new(MAC_THREAD_TABLE_SIZE);
    _regs  = hash_table_new(MAC_THREAD_TABLE_SIZE);

    if (!isvalid(_ports) || !isvalid(_idle) || !isvalid(_int) || !isvalid(_regs)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate macOS thread state tables");
        hash_table__destroy(_ports);
        hash_table__destroy(_idle);
        hash_table__destroy(_int);
        hash_table__destroy(_regs);
        _ports = _idle = _int = _regs = NULL;
        FAIL;
    } // GCOV_EXCL_STOP
    SUCCESS;
}

static void
_py_thread_free_native(void) {
    // Release all cached Mach thread ports before destroying the table.
    if (isvalid(_ports)) {
        hash_table__iter_start(_ports, void*, raw_port) {
            mach_port_deallocate(mach_task_self(), (thread_act_t)(uintptr_t)raw_port);
        }
        hash_table__iter_stop(_ports);
    }
    hash_table__destroy(_ports);
    hash_table__destroy(_idle);
    hash_table__destroy(_int);
    if (isvalid(_regs)) {
        hash_table__iter_start(_regs, thread_regs_t*, regs) { free(regs); }
        hash_table__iter_stop(_regs);
    }
    hash_table__destroy(_regs);
    _ports = _idle = _int = _regs = NULL;
}
