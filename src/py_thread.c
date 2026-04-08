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

#define PY_THREAD_C

#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "argparse.h"
#include "cache.h"
#include "error.h"
#include "events.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "platform.h"
#include "stack.h"
#include "timing.h"
#include "version.h"

#include "py_thread.h"

// ---- PRIVATE ---------------------------------------------------------------

static size_t max_pid = 0;

// Platform-specific NATIVE thread state storage.
// These are declared here so they are visible to the platform headers below.
#if defined(NATIVE) && defined(PL_LINUX)
static void**         _tids      = NULL; // libunwind-ptrace contexts, indexed by kernel TID
static unsigned char* _tids_idle = NULL; // idle-state bitmap, indexed by kernel TID
static unsigned char* _tids_int  = NULL; // interrupted-state bitmap, indexed by kernel TID
static char**         _kstacks   = NULL; // kernel stack strings, indexed by kernel TID
#elif defined(NATIVE) && defined(PL_MACOS)
static hash_table_t* _mac_ports = NULL; // pthread_t → thread_act_t (Mach thread port)
static hash_table_t* _mac_idle  = NULL; // pthread_t → (void*)1  if thread was idle before suspend
static hash_table_t* _mac_int   = NULL; // pthread_t → (void*)1  if thread was suspended by us
#endif

// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of py_thread__is_idle
// ----------------------------------------------------------------------------

#if defined(PL_LINUX)

#include "linux/py_thread.h"
#if defined NATIVE && defined HAVE_BFD
#include "linux/addr2line.h"
#endif

#elif defined(PL_WIN)

#include "win/py_thread.h"

#elif defined(PL_MACOS)

#include "mac/py_thread.h"
#ifdef NATIVE
#include "mac/addr2line.h"
#include "mac/common.h"
#endif

#endif

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
static inline int
_py_thread__resolve_py_stack(py_thread_t* self) {
    lru_cache_t* cache = self->proc->frame_cache;

    for (int i = 0; i < stack_pointer(); i++) {
        py_frame_t py_frame = stack_py_get(i);

#ifdef NATIVE
        if (py_frame.origin == CFRAME_MAGIC) {
            stack_set(i, CFRAME_MAGIC);
            continue;
        }
#endif
        int      lasti     = py_frame.lasti;
        key_dt   frame_key = py_frame_key(py_frame.code, lasti);
        frame_t* frame     = lru_cache__maybe_hit(cache, frame_key);

        if (!isvalid(frame)) {
            frame = _frame_remote(self->proc, py_frame.code, lasti);
            if (!isvalid(frame)) {
                // Truncate the stack to the point where we have successfully resolved.
                _stack->pointer = i;
                FAIL;
            }
            lru_cache__store(cache, frame_key, frame);

            event_handler__emit_new_frame(frame);
        }

        stack_set(i, frame);
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__push_remote_frame(py_thread_t* self, raddr_t* prev) {
    PyFrameObject frame;

    if (fail(copy_remote_v(self->proc->ref, *prev, frame, self->proc->py_v->py_frame.size)))
        FAIL;

    V_DESC(self->proc->py_v);

    raddr_t origin = *prev;

    *prev = V_FIELD(raddr_t, frame, py_frame, o_back);
    if (unlikely(origin == *prev)) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Frame points to itself");
        FAIL;
    } // GCOV_EXCL_STOP

    stack_py_push(origin, V_FIELD(raddr_t, frame, py_frame, o_code), V_FIELD(int, frame, py_frame, o_lasti));

    SUCCESS;
}

// ----------------------------------------------------------------------------
#define REL(raddr, block, base) (raddr - block.lo + base)

#ifdef DEBUG
static unsigned int _stack_chunk_count  = 0;
static unsigned int _stack_chunk_misses = 0;
#endif

// ----------------------------------------------------------------------------
static inline int
_py_thread__push_local_iframe(py_thread_t* self, void* iframe, raddr_t* prev) {
    V_DESC(self->proc->py_v);

    raddr_t origin     = *prev;
    raddr_t code_raddr = V_FIELD_PTR(raddr_t, iframe, py_iframe, o_code);

    *prev = V_FIELD_PTR(raddr_t, iframe, py_iframe, o_previous);
    if (unlikely(origin == *prev)) {
        set_error(PYOBJECT, "Interpreter frame points to itself");
        FAIL;
    }

    if (V_MIN(3, 12) && V_FIELD_PTR(char, iframe, py_iframe, o_owner) == FRAME_OWNED_BY_CSTACK) {
// This is a shim frame that we can ignore
#ifdef NATIVE
        // In native mode we take this as the marker for the beginning of the stack
        // for a call to PyEval_EvalFrameDefault.
        stack_py_push_cframe();
#endif
        SUCCESS;
    }

    stack_py_push(
        origin, code_raddr,
        (((int)(V_FIELD_PTR(raddr_t, iframe, py_iframe, o_prev_instr) - code_raddr)) - py_v->py_code.o_code)
            / sizeof(_Py_CODEUNIT)
    );

#ifdef NATIVE
    if (V_EQ(3, 11) && V_FIELD_PTR(int, iframe, py_iframe, o_is_entry)) {
        // This marks the end of a CFrame
        stack_py_push_cframe();
    }
#endif

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__push_remote_iframe(py_thread_t* self, raddr_t* prev) {
    V_DESC(self->proc->py_v);

    V_ALLOCA(iframe, iframe);

    if (fail(copy_py(self->proc->ref, *prev, py_iframe, iframe)))
        FAIL;

    return _py_thread__push_local_iframe(self, &iframe, prev);
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__push_iframe(py_thread_t* self, raddr_t* prev) {
    raddr_t raddr = *prev;
    if (isvalid(self->stack)) {
#ifdef DEBUG
        _stack_chunk_count++;
#endif

        void* resolved_addr = isvalid(self->stack) ? stack_chunk__resolve(self->stack, raddr) : NULL;
        if (isvalid(resolved_addr)) {
            return _py_thread__push_local_iframe(self, resolved_addr, prev);
        }

#ifdef DEBUG
        _stack_chunk_misses++;
#endif
    }

    return _py_thread__push_remote_iframe(self, prev);
} /* _py_thread__push_iframe */

// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_frame_stack(py_thread_t* self) {
    stack_reset();

    raddr_t prev = self->top_frame;

    while (isvalid(prev)) {
        if (fail(_py_thread__push_remote_frame(self, &prev))) {
            log_d("Failed to retrieve frame #%d (from top).", stack_pointer());
            FAIL;
        }
        if (stack_full()) { // GCOV_EXCL_START
            log_w("Invalid frame stack: too tall");
            FAIL;
        } // GCOV_EXCL_STOP
        if (stack_has_cycle()) {
            log_d("Circular frame reference detected");
            FAIL;
        }
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_iframe_stack(py_thread_t* self, raddr_t iframe_raddr) {
    raddr_t curr = iframe_raddr;

    while (isvalid(curr)) {
        if (fail(_py_thread__push_iframe(self, &curr))) {
            log_d("Failed to retrieve iframe #%d", stack_pointer());
            FAIL;
        }

        if (stack_full()) { // GCOV_EXCL_START
            log_w("Invalid frame stack: too tall");
            FAIL;
        } // GCOV_EXCL_STOP

        if (stack_has_cycle()) {
            log_d("Circular frame reference detected");
            FAIL;
        }
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_cframe_stack(py_thread_t* self) {
    PyCFrame cframe;

    stack_reset();

    V_DESC(self->proc->py_v);

    if (fail(copy_py(self->proc->ref, self->top_frame, py_cframe, cframe)))
        FAIL;

    return fail(_py_thread__unwind_iframe_stack(self, V_FIELD(raddr_t, cframe, py_cframe, o_current_frame)));
}

#ifdef NATIVE

// ============================================================
// Shared NATIVE helpers: idle/interrupted state management.
// Linux uses bitmaps indexed by kernel TID (small integer).
// macOS uses hash tables keyed by pthread_t (pointer value).
// ============================================================

// ----------------------------------------------------------------------------
int
py_thread__set_idle(py_thread_t* self) {
#if defined(PL_LINUX)
    unsigned char bit   = 1 << (self->tid & 7);
    size_t        index = self->tid >> 3;

    if (index > (max_pid >> 3)) { // GCOV_EXCL_START
        set_error(OS, "Invalid thread identifier");
        FAIL;
    } // GCOV_EXCL_STOP

    if (py_thread__is_idle(self)) {
        _tids_idle[index] |= bit;
    } else {
        _tids_idle[index] &= ~bit;
    }
#elif defined(PL_MACOS)
    // Query idle state now, before the thread is suspended.
    if (_mac_thread__is_idle_now(self)) {
        hash_table__set(_mac_idle, (key_dt)self->tid, (value_t)1);
    } else {
        hash_table__del(_mac_idle, (key_dt)self->tid);
    }
#endif
    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__set_interrupted(py_thread_t* self, bool state) {
#if defined(PL_LINUX)
    unsigned char bit   = 1 << (self->tid & 7);
    size_t        index = self->tid >> 3;

    if (state) {
        _tids_int[index] |= bit;
    } else {
        _tids_int[index] &= ~bit;
    }
#elif defined(PL_MACOS)
    if (state) {
        hash_table__set(_mac_int, (key_dt)self->tid, (value_t)1);
    } else {
        hash_table__del(_mac_int, (key_dt)self->tid);
    }
#endif
    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__is_interrupted(py_thread_t* self) {
#if defined(PL_LINUX)
    return _tids_int[self->tid >> 3] & (1 << (self->tid & 7));
#elif defined(PL_MACOS)
    return isvalid(hash_table__get(_mac_int, (key_dt)self->tid));
#endif
    return 0;
}

// ============================================================
// Linux-only: kernel stack capture
// ============================================================

#ifdef PL_LINUX

// ----------------------------------------------------------------------------
// Resume every thread whose interrupted bit is set by scanning the bitmap
// directly rather than re-traversing the Python linked list. This correctly
// handles threads that were removed from the list between the interrupt and
// resume phases (e.g. a thread that exited mid-sample), which a linked-list
// walk would silently miss, leaving those threads stuck in ptrace-stop.
void
py_thread__resume_all_interrupted(void) {
    size_t bmsize = (max_pid >> 3) + 1;

    for (size_t i = 0; i < bmsize; i++) {
        if (!_tids_int[i])
            continue;
        for (int b = 0; b < 8; b++) {
            unsigned char bit = (unsigned char)(1 << b);
            if (!(_tids_int[i] & bit))
                continue;
            pid_t tid = (pid_t)((i << 3) | b);
            if (ptrace(PTRACE_CONT, tid, 0, 0)) {
                log_d("ptrace: failed to resume thread %d (errno: %d)", tid, errno);
            } else {
                log_t("ptrace: thread %d resumed", tid);
            }
            _tids_int[i] &= ~bit; // always clear so the thread isn't stuck
        }
    }
}

// ----------------------------------------------------------------------------
#define MAX_STACK_FILE_SIZE 2048

int
py_thread__save_kernel_stack(py_thread_t* self) {
    char stack_path[48];

    if (!isvalid(_kstacks)) { // GCOV_EXCL_START
        set_error(NULL, "Kernel stacks not initialized");
        FAIL;
    } // GCOV_EXCL_STOP

    sfree(_kstacks[self->tid]);

    sprintf(stack_path, "/proc/%d/task/%" PRIuPTR "/stack", self->proc->pid, self->tid);
    cu_fd fd = open(stack_path, O_RDONLY);
    if (fd == -1) { // GCOV_EXCL_START
        set_error(IO, "Failed to open kernel stack file");
        FAIL;
    } // GCOV_EXCL_STOP

    _kstacks[self->tid] = (char*)calloc(1, MAX_STACK_FILE_SIZE);
    if (!isvalid(_kstacks[self->tid])) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate kernel stack buffer");
        FAIL;
    } // GCOV_EXCL_STOP

    if (read(fd, _kstacks[self->tid], MAX_STACK_FILE_SIZE) == -1) { // GCOV_EXCL_START
        set_error(IO, "Failed to read kernel stack file");
        FAIL;
    } // GCOV_EXCL_STOP

    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline int
_py_thread__unwind_kernel_frame_stack(py_thread_t* self) {
    char* line = _kstacks[self->tid];
    if (!isvalid(line)) // GCOV_EXCL_LINE
        SUCCESS;        // GCOV_EXCL_LINE

    log_t("linux: unwinding kernel stack");

    stack_kernel_reset();

    for (;;) {
        char* eol = strchr(line, '\n');
        if (!isvalid(eol))
            break;
        *eol = '\0';

        char* b = strchr(line, ']');
        if (isvalid(b)) {
            char* e = strchr(++b, '+');
            if (isvalid(e))
                *e = 0;

            stack_kernel_push(strdup(++b));
        }
        line = eol + 1;
    }

    SUCCESS;
}

#endif /* PL_LINUX */

// ============================================================
// Native stack unwinding — platform-specific implementations
// ============================================================

static char _native_buf[MAXLEN];

// ----------------------------------------------------------------------------
// Linux: remote unwinding via libunwind-ptrace
// ----------------------------------------------------------------------------
#if defined(PL_LINUX)

static inline int
wait_unw_init_remote(unw_cursor_t* c, unw_addr_space_t as, void* arg) {
    int outcome = unw_init_remote(c, as, arg);
    if (fail(outcome))
        log_e("unwind: failed to initialize cursor (%d)", outcome);
    return outcome;
}

static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    unw_cursor_t cursor;
    unw_word_t   offset, pc;

    lru_cache_t* cache        = self->proc->frame_cache;
    lru_cache_t* string_cache = self->proc->string_cache;
    void*        context      = _tids[self->tid];

    stack_native_reset();

    if (!isvalid(context)) { // GCOV_EXCL_START
        _tids[self->tid] = _UPT_create(self->tid);
        if (!isvalid(_tids[self->tid])) {
            set_error(OS, "Failed to create libunwind context");
            FAIL;
        }
        if (!isvalid(context)) {
            set_error(OS, "Unexpected invalid context");
            FAIL;
        }
    } // GCOV_EXCL_STOP

    if (fail(wait_unw_init_remote(&cursor, self->proc->unwind.as, context))) {
        set_error(OS, "Failed to initialize remote cursor");
        FAIL;
    }

    do {
        if (unw_get_reg(&cursor, UNW_REG_IP, &pc)) { // GCOV_EXCL_START
            set_error(OS, "Failed to read program counter");
            FAIL;
        } // GCOV_EXCL_STOP

        key_dt frame_key = (key_dt)pc;

        frame_t* frame = lru_cache__maybe_hit(cache, frame_key);
        if (!isvalid(frame)) {
            cached_string_t* scope    = NULL;
            cached_string_t* filename = NULL;
            vm_range_t*      range    = NULL;
            if (pargs.where) {
                range = vm_range_tree__find(self->proc->maps_tree, pc);
// TODO: A failed attempt to find a range is an indication that we need
// to regenerate the VM maps. This would be of no use at the moment,
// since we only use them in `where` mode where we sample just once. If
// we resort to improving addr2line and use the VM range tree for
// normal mode, then we should consider catching the case
// !isvalid(range) and regenerate the VM range tree with fresh data.
#ifdef HAVE_BFD
                if (isvalid(range)) {
                    unw_word_t base = (unw_word_t)hash_table__get(self->proc->base_table, string__hash(range->name));
                    if (base > 0)
                        frame = get_native_frame(range->name, pc - base, frame_key);
                }
#endif
            }
            if (!isvalid(frame)) {
                unw_proc_info_t pi;
                if (success(unw_get_proc_info(&cursor, &pi))) {
                    key_dt scope_key = (key_dt)pi.start_ip;
                    scope            = lru_cache__maybe_hit(string_cache, scope_key);
                    if (!isvalid(scope)) {
                        if (unw_get_proc_name(&cursor, _native_buf, MAXLEN, &offset) == 0) {
                            scope = cached_string_new(scope_key, strdup(_native_buf));
                            if (!isvalid(scope)) {
                                FAIL; // GCOV_EXCL_LINE
                            }
                            lru_cache__store(string_cache, scope_key, (value_t)scope);
                            event_handler__emit_new_string(scope);
                        }
                    }
                }
                if (!isvalid(scope)) {
                    scope  = UNKNOWN_SCOPE;
                    offset = 0;
                }

                if (isvalid(range)) { // For now this is only relevant in `where` mode
                    filename = cached_string_new((key_dt)pc, range->name);
                    if (!isvalid(filename)) {
                        FAIL; // GCOV_EXCL_LINE
                    }
                } else {
                    // The program counter carries information about the file name *and*
                    // the line number. Given that we don't resolve the file name using
                    // memory ranges at runtime for performance reasons, we need to store
                    // the PC value so that we can later resolve it to a file name and
                    // line number, instead of doing the more sensible thing of using
                    // something like `scope_key+1`, or the resolved base address.
                    key_dt filename_key = (key_dt)pc;
                    filename            = lru_cache__maybe_hit(string_cache, filename_key);
                    if (!isvalid(filename)) {
                        sprintf(_native_buf, "native@%" PRIxPTR, pc);
                        filename = cached_string_new(filename_key, strdup(_native_buf));
                        if (!isvalid(filename)) {
                            FAIL; // GCOV_EXCL_LINE
                        }
                        lru_cache__store(string_cache, filename_key, (value_t)filename);
                        event_handler__emit_new_string(filename);
                    }
                }

                frame = frame_new(frame_key, filename, scope, offset, 0, 0, 0);
                if (!isvalid(frame)) // GCOV_EXCL_LINE
                    FAIL;            // GCOV_EXCL_LINE
            }

            lru_cache__store(cache, frame_key, (value_t)frame);

            event_handler__emit_new_frame(frame);
        }

        stack_native_push(frame);
    } while (!stack_native_full() && unw_step(&cursor) > 0);

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack */

// ----------------------------------------------------------------------------
// Linux: ptrace-based thread seize
// ----------------------------------------------------------------------------

static inline int
_py_thread__seize(py_thread_t* self) {
    if (isvalid(_tids[self->tid])) {
        // A context already exists for this TID. Verify the thread is still
        // alive to detect TID reuse: if the original thread exited and a new
        // thread was assigned the same TID, the old libunwind context is stale
        // and must be replaced before we can safely unwind the new thread.
        char task_path[48];
        sprintf(task_path, "/proc/%d/task/%" PRIuPTR, self->proc->pid, self->tid);
        if (access(task_path, F_OK) == 0)
            SUCCESS; // Same thread still alive; context is valid

        // Thread exited — tear down the stale context and re-seize the new one.
        log_d("ptrace: TID %" PRIuPTR " reused, releasing stale context", self->tid);
        _UPT_destroy(_tids[self->tid]);
        _tids[self->tid]            = NULL;
        _tids_int[self->tid >> 3]  &= ~(1 << (self->tid & 7));
        _tids_idle[self->tid >> 3] &= ~(1 << (self->tid & 7));
    }

    if (fail(wait_ptrace_seize(self->tid))) { // GCOV_EXCL_START
        set_error(OS, "Failed to seize thread");
        FAIL;
    } // GCOV_EXCL_STOP

    log_d("ptrace: thread %" PRIuPTR " seized", self->tid);
    _tids[self->tid] = _UPT_create(self->tid);
    if (!isvalid(_tids[self->tid])) { // GCOV_EXCL_START
        set_error(OS, "Failed to create libunwind context");
        FAIL;
    } // GCOV_EXCL_STOP

    SUCCESS;
}

// ----------------------------------------------------------------------------
// macOS: frame-pointer walk via Mach APIs
// ----------------------------------------------------------------------------
#elif defined(PL_MACOS)

// Mask off pointer-authentication bits from a return address on arm64.
// User-space VAs on Apple Silicon are at most 39 bits wide.
#if defined(__arm64__)
#define _MAC_STRIP_PAC(addr) ((uintptr_t)(addr) & 0x0000007fffffffffull)
#else
#define _MAC_STRIP_PAC(addr) ((uintptr_t)(addr))
#endif

// Find the Mach thread port for self->tid (a pthread_t value) and cache it
// in _mac_ports.  Idempotent: does nothing if the port is already cached.
int
_mac_thread_seize(py_thread_t* self) {
    if (isvalid(hash_table__get(_mac_ports, (key_dt)self->tid)))
        SUCCESS;

    // _silly_offset adjusts pthread_t to the value stored in thread_handle.
    // It is initialised by _mac_thread__is_idle_now(), called before us in
    // _py_proc__interrupt_threads.  Fall back to SILLY_OFFSET if not yet set.
    if (unlikely(_silly_offset == 0))
        _infer_thread_id_offset(self);

    thread_act_t port = _mac_find_thread_port(self->proc->ref, self->tid + _silly_offset);
    if (port == MACH_PORT_NULL) {
        set_error(OS, "Failed to find Mach thread port");
        FAIL;
    }

    hash_table__set(_mac_ports, (key_dt)self->tid, (value_t)(uintptr_t)port);
    log_d("mac: seized thread %p → port %u", (void*)self->tid, port);
    SUCCESS;
}

// Suspend the thread.  Locates and caches the Mach port if not already done.
int
py_thread__suspend(py_thread_t* self) {
    if (fail(_mac_thread_seize(self)))
        FAIL;

    thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_mac_ports, (key_dt)self->tid);
    if (thread_suspend(port) != KERN_SUCCESS) {
        set_error(OS, "thread_suspend failed");
        FAIL;
    }
    SUCCESS;
}

// Resume the thread.  No-op if the port was never cached.
int
py_thread__resume(py_thread_t* self) {
    thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_mac_ports, (key_dt)self->tid);
    if (!port)
        SUCCESS;

    if (thread_resume(port) != KERN_SUCCESS) {
        set_error(OS, "thread_resume failed");
        FAIL;
    }
    SUCCESS;
}

// Resume all interrupted threads without walking the Python thread linked list.
// Iterates _mac_int directly (avoids N copy_remote calls).
// The hash_table iterator is not mutation-safe, so collect TIDs first.
void
py_thread__resume_all_interrupted(void) {
    key_dt interrupted[256];
    int    n = 0;
    hash_table__iteritems_start(_mac_int, key_dt, _itid, void*, _sentinel) {
        (void)_sentinel;
        if (n < 256)
            interrupted[n++] = _itid;
    }
    hash_table__iter_stop(_mac_int);

    for (int i = 0; i < n; i++) {
        key_dt       rtid = interrupted[i];
        thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_mac_ports, rtid);
        if (port != MACH_PORT_NULL) {
            if (thread_resume(port) != KERN_SUCCESS) {
                log_d("mac: thread_resume failed for port %u", port);
            } else {
                log_t("mac: thread %p resumed", (void*)rtid);
            }
        }
        hash_table__del(_mac_int, rtid);
    }
}

// Walk the native call stack of a suspended thread using the frame-pointer
// chain.  Initial registers are obtained via thread_get_state(); subsequent
// frames are read from the remote address space with mach_vm_read_overwrite().
//
// The filename is set to the path of the mapped binary obtained via
// proc_regionfilename(), or "native@<pc>" when the mapping is unknown.
// Scope (function name) is resolved via mac_get_func_name() which reads
// the Mach-O LC_SYMTAB and performs an ASLR-adjusted binary search.
static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    thread_act_t port = (thread_act_t)(uintptr_t)hash_table__get(_mac_ports, (key_dt)self->tid);
    if (!port) {
        set_error(OS, "No Mach thread port cached for thread");
        FAIL;
    }

    stack_native_reset();

    // ---- Seed registers from thread state ----
    uintptr_t pc, fp, sp;

#if defined(__x86_64__)
    x86_thread_state64_t   state = {0};
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(port, x86_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
        set_error(OS, "thread_get_state failed");
        FAIL;
    }
    pc = (uintptr_t)state.__rip;
    fp = (uintptr_t)state.__rbp;
    sp = (uintptr_t)state.__rsp;
#elif defined(__arm64__)
    arm_thread_state64_t   state = {0};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &count) != KERN_SUCCESS) {
        set_error(OS, "thread_get_state failed");
        FAIL;
    }
    pc = (uintptr_t)arm_thread_state64_get_pc(state);
    fp = (uintptr_t)arm_thread_state64_get_fp(state);
    sp = (uintptr_t)arm_thread_state64_get_sp(state);
#else
#error "Unsupported architecture for macOS native stack walking"
#endif

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
                path_len = proc_regionfilename(self->proc->pid, pc, region_path, MAXPATHLEN);
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
                if (path_len < 0)
                    path_len = proc_regionfilename(self->proc->pid, pc, region_path, MAXPATHLEN);
                const char* fname = NULL;
                if (path_len > 0)
                    fname = mac_get_func_name(self->proc->ref, self->proc->pid, pc, region_path);
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
            && fp + sizeof(frame_data) <= _stack_buf_base + _STACK_BUF_SIZE) {
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
        pc = _MAC_STRIP_PAC(frame_data[1]);
    }
#undef _STACK_BUF_SIZE

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack */

#endif /* PL_LINUX / PL_MACOS */

#endif /* NATIVE */

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
py_thread__read_remote(py_thread_t* self, raddr_t addr) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid thread pointer");
        FAIL;
    } // GCOV_EXCL_STOP

    py_proc_t* proc = self->proc;

    V_DESC(proc->py_v);

    V_ALLOCA(thread, ts);

    if (fail(copy_remote(proc->ref, addr, ts))) {
        FAIL;
    }

    self->stack = NULL;
    if (V_MIN(3, 11)) {
        // This is destroyed in py_thread__next, so it is important that all threads
        // are traversed to avoid a memory leak!
        self->stack = stack_chunk_new(proc->ref, V_FIELD(raddr_t, ts, py_thread, o_stack));
    }

    self->addr      = addr;
    self->top_frame = V_FIELD(raddr_t, ts, py_thread, o_frame);
    self->status    = V_FIELD(tstate_status_t, ts, py_thread, o_status);
    self->next      = V_FIELD(raddr_t, ts, py_thread, o_next) == addr ? NULL : V_FIELD(raddr_t, ts, py_thread, o_next);

#if defined PL_MACOS
    self->tid = V_FIELD(long, ts, py_thread, o_thread_id);
#else
    if (V_MIN(3, 11)) {
        self->tid = V_FIELD(long, ts, py_thread, o_native_thread_id);
    } else {
        self->tid = V_FIELD(long, ts, py_thread, o_thread_id);
    }
#endif
    if (self->tid == 0) {
        set_error(OS, "Cannot retrieve native thread ID information");
        FAIL;
    }
#if defined PL_LINUX
    else {
        if (V_MIN(3, 11)) {
// We already have the native thread id
#ifdef NATIVE
            if (fail(_py_thread__seize(self))) { // GCOV_EXCL_START
                FAIL;
            } // GCOV_EXCL_STOP
#endif
        } else if (likely(proc->extra->pthread_tid_offset) && success(read_pthread_t(self->proc, (void*)self->tid))) {
            int o     = proc->extra->pthread_tid_offset;
            self->tid = o > 0 ? proc->extra->_pthread_buffer[o] : (pid_t)((pid_t*)proc->extra->_pthread_buffer)[-o];
            if (self->tid >= max_pid || self->tid == 0) {
                log_e("Invalid TID detected");
                self->tid = 0;
                FAIL;
            }
#ifdef NATIVE
            if (fail(_py_thread__seize(self))) {
                FAIL;
            }
#endif
        }
    }
#endif

    SUCCESS;
} /* py_thread__read_remote */

// ----------------------------------------------------------------------------
int
py_thread__next(py_thread_t* self) {
    V_DESC(self->proc->py_v);

    if (V_MIN(3, 11)) {
        stack_chunk__destroy(self->stack);
        self->stack = NULL;
    }

    if (!isvalid(self->next))
        STOP(ITEREND);

    log_t("Found next thread");

    return py_thread__read_remote(self, self->next);
}

// ----------------------------------------------------------------------------
void
py_thread__unwind(py_thread_t* self) {
    bool error = false;

#ifdef NATIVE

#ifdef PL_LINUX
    // Only unwind the native stack if this thread was stopped during the
    // interrupt phase. A thread that appears in the Python linked list but
    // was NOT interrupted (i.e. it was created after we finished interrupting)
    // is still running — unwinding its registers would produce garbage or
    // crash libunwind.
    if (py_thread__is_interrupted(self)) {
        // We sample the kernel frame stack BEFORE interrupting because
        // otherwise we would see the ptrace syscall call stack, which is not
        // very interesting. The downside is that the kernel stack might not be
        // in sync with the other ones.
        if (pargs.kernel) {
            _py_thread__unwind_kernel_frame_stack(self);
        }
        if (fail(_py_thread__unwind_native_frame_stack(self))) {
            error = true;
        }
    }
#endif /* PL_LINUX */

#ifdef PL_MACOS
    if (pargs_native && fail(_py_thread__unwind_native_frame_stack(self))) {
        error = true;
    }
    // No re-read here: the thread is suspended (thread_suspend) so its
    // state cannot change between _py_proc__interrupt_threads and now.
#endif /* PL_MACOS */

#endif /* NATIVE */
    V_DESC(self->proc->py_v);

    if (isvalid(self->top_frame)) {
        if (V_MIN(3, 13)) {
            if (fail(_py_thread__unwind_iframe_stack(self, self->top_frame))) {
                error = true;
            }
        } else if (V_MIN(3, 11)) {
            if (fail(_py_thread__unwind_cframe_stack(self))) {
                error = true;
            }
        } else {
            if (fail(_py_thread__unwind_frame_stack(self))) {
                error = true;
            }
        }

        if (fail(_py_thread__resolve_py_stack(self))) {
            error = true;
        }
    }

    // Update sampling stats
    stats_count_sample();
    if (error)
        stats_count_error();
    stats_check_duration(stopwatch_duration());
}

// ----------------------------------------------------------------------------
int
py_thread_allocate(void) {
    if (isvalid(_stack)) // GCOV_EXCL_LINE
        SUCCESS;         // GCOV_EXCL_LINE

    if (fail(stack_allocate(MAX_STACK_SIZE))) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

#if defined PL_WIN
    // On Windows we need to fetch process and thread information to detect idle
    // threads. We allocate a buffer for periodically fetching that data and, if
    // needed we grow it at runtime.
    _pi_buffer_size = (1 << 16) * sizeof(void*);
    _pi_buffer      = calloc(1, _pi_buffer_size);
    if (!isvalid(_pi_buffer)) {
        set_error(MALLOC, "Failed to allocate process information buffer");
        FAIL;
    }
#endif

    max_pid = pid_max() + 1;

#if defined(NATIVE) && defined(PL_LINUX)
    _tids = (void**)calloc(max_pid, sizeof(void*));
    if (!isvalid(_tids)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate thread context buffer");
        goto failed;
    } // GCOV_EXCL_STOP

    size_t bmsize = (max_pid >> 3) + 1;

    _tids_idle = (unsigned char*)calloc(bmsize, sizeof(unsigned char));
    if (!isvalid(_tids_idle)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate thread idle bitmap");
        goto failed;
    } // GCOV_EXCL_STOP

    _tids_int = (unsigned char*)calloc(bmsize, sizeof(unsigned char));
    if (!isvalid(_tids_int)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate thread internal bitmap");
        goto failed;
    } // GCOV_EXCL_STOP

    if (pargs.kernel) {
        _kstacks = (char**)calloc(max_pid, sizeof(char*));
        if (!isvalid(_kstacks)) { // GCOV_EXCL_START
            set_error(MALLOC, "Failed to allocate kernel stack buffer");
            goto failed;
        } // GCOV_EXCL_STOP
    }
    goto ok;

failed: // GCOV_EXCL_START
    sfree(_tids);
    sfree(_tids_idle);
    sfree(_tids_int);
    sfree(_kstacks);

    FAIL;

ok:    // GCOV_EXCL_STOP
#endif /* defined(NATIVE) && defined(PL_LINUX) */

#if defined(NATIVE) && defined(PL_MACOS)
#define MAC_THREAD_TABLE_SIZE 256
    _mac_ports = hash_table_new(MAC_THREAD_TABLE_SIZE);
    _mac_idle  = hash_table_new(MAC_THREAD_TABLE_SIZE);
    _mac_int   = hash_table_new(MAC_THREAD_TABLE_SIZE);

    if (!isvalid(_mac_ports) || !isvalid(_mac_idle) || !isvalid(_mac_int)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate macOS thread state tables");
        hash_table__destroy(_mac_ports);
        hash_table__destroy(_mac_idle);
        hash_table__destroy(_mac_int);
        _mac_ports = _mac_idle = _mac_int = NULL;
        FAIL;
    } // GCOV_EXCL_STOP
#endif /* defined(NATIVE) && defined(PL_MACOS) */

    SUCCESS;
}

// ----------------------------------------------------------------------------
void
py_thread_free(void) {
#if defined PL_WIN
    sfree(_pi_buffer);
#endif

#ifdef DEBUG
    if (_stack_chunk_count) {
        log_d(
            "Stack chunk hit ratio: %d/%d (%0.2f%%)\n", _stack_chunk_count - _stack_chunk_misses, _stack_chunk_count,
            (_stack_chunk_count - _stack_chunk_misses) * 100.0 / _stack_chunk_count
        );
    }
#endif

    stack_deallocate();

#if defined(NATIVE) && defined(PL_LINUX)
    for (pid_t tid = 0; tid < max_pid; tid++) {
        if (isvalid(_tids[tid])) {
            _UPT_destroy(_tids[tid]);
            if (fail(wait_ptrace(PTRACE_DETACH, tid, 0, 0))) {
                log_d("ptrace: failed to detach thread %ld", tid);
            } else {
                log_d("ptrace: thread %ld detached", tid);
            }
        }
        if (isvalid(_kstacks) && isvalid(_kstacks[tid])) {
            sfree(_kstacks[tid]);
        }
    }
    sfree(_tids);
    sfree(_tids_idle);
    sfree(_tids_int);
    sfree(_kstacks);
#endif /* defined(NATIVE) && defined(PL_LINUX) */

#if defined(NATIVE) && defined(PL_MACOS)
    // Release all cached Mach thread ports before destroying the table.
    if (isvalid(_mac_ports)) {
        hash_table__iter_start(_mac_ports, void*, raw_port) {
            mach_port_deallocate(mach_task_self(), (thread_act_t)(uintptr_t)raw_port);
        }
        hash_table__iter_stop(_mac_ports);
    }
    hash_table__destroy(_mac_ports);
    hash_table__destroy(_mac_idle);
    hash_table__destroy(_mac_int);
    _mac_ports = _mac_idle = _mac_int = NULL;
#endif /* defined(NATIVE) && defined(PL_MACOS) */
}
