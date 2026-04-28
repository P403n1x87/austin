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
#include "version.h"

#include "py_thread.h"

// ---- PRIVATE ---------------------------------------------------------------

static size_t max_pid = 0;

// Buffer for formatting native frame filenames. Only needed on platforms /
// architectures that actually perform native unwinding. Declared here so it
// is visible to the platform headers included below.
#if !defined(PL_LINUX) || defined(AUSTINP) || defined(__x86_64__) || defined(__aarch64__)
static char _native_buf[MAXLEN];
#endif

// ----------------------------------------------------------------------------
// -- Platform-dependent implementations.
// ----------------------------------------------------------------------------

#if defined(PL_LINUX)

#include "linux/addr2line.h"
#include "linux/py_thread.h"

#elif defined(PL_WIN)

#include "win/addr2line.h"
#include "win/py_thread.h"

#elif defined(PL_MACOS)

#include "mac/addr2line.h"
#include "mac/common.h"
#include "mac/py_thread.h"

#endif

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
static inline int
_py_thread__resolve_py_stack(py_thread_t* self) {
    lru_cache_t* cache = self->proc->frame_cache;

    for (int i = 0; i < stack_pointer(); i++) {
        py_frame_t py_frame = stack_py_get(i);

        if (py_frame.origin == CFRAME_MAGIC) {
            stack_set(i, CFRAME_MAGIC);
            continue;
        }
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
        // This is a shim frame that we can ignore.
        // In native mode we take this as the marker for the beginning of the stack
        // for a call to PyEval_EvalFrameDefault.
        if (pargs_native)
            stack_py_push_cframe();
        SUCCESS;
    }

    stack_py_push(
        origin, code_raddr,
        (((int)(V_FIELD_PTR(raddr_t, iframe, py_iframe, o_prev_instr) - code_raddr)) - py_v->py_code.o_code)
            / sizeof(_Py_CODEUNIT)
    );

    if (pargs_native && V_EQ(3, 11) && V_FIELD_PTR(int, iframe, py_iframe, o_is_entry)) {
        // This marks the end of a CFrame
        stack_py_push_cframe();
    }

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

    V_DESC(self->proc->py_v);

    if (fail(copy_py(self->proc->ref, self->top_frame, py_cframe, cframe)))
        FAIL;

    return fail(_py_thread__unwind_iframe_stack(self, V_FIELD(raddr_t, cframe, py_cframe, o_current_frame)));
}

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
// Read the code object pointer from the top frame without copying the full
// frame struct. For < 3.11 reads PyFrameObject.f_code; for >= 3.11 reads
// _PyInterpreterFrame.f_code. Returns NULL on failure (treated as unknown).
static inline void*
_py_thread__read_top_code(py_thread_t* self) {
    V_DESC(self->proc->py_v);

    raddr_t code = NULL;
    int     offset;

    if (V_MIN(3, 11)) {
        offset = py_v->py_iframe.o_code;
    } else {
        offset = py_v->py_frame.o_code;
    }

    copy_memory(self->proc->ref, (char*)self->top_frame + offset, sizeof(raddr_t), &code);
    return code;
}

// ----------------------------------------------------------------------------
// Core thread-state read: copies the remote thread state and extracts all
// fields except the datastack chunk.  Declared static inline so that both
// py_thread__read_remote and py_thread__read_with_stack_remote can call it
// without function-call overhead.
static inline int
_py_thread__read_remote(py_thread_t* self, raddr_t addr) {
    py_proc_t* proc = self->proc;

    V_DESC(proc->py_v);

    V_ALLOCA(thread, ts);

    if (fail(copy_remote(proc->ref, addr, ts))) {
        FAIL;
    }

    self->stack       = NULL;
    self->stack_raddr = NULL;
    if (V_MIN(3, 11)) {
        self->stack_raddr = V_FIELD(raddr_t, ts, py_thread, o_stack);
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
            // We already have the native thread id.  Validate before indexing
            // the bitmap or passing to ptrace.
            if (pargs_native && (uintptr_t)self->tid >= (uintptr_t)max_pid) { // GCOV_EXCL_START
                log_t("native TID %" PRIuPTR " out of range, skipping thread", self->tid);
                FAIL;
            } // GCOV_EXCL_STOP
        } else if (likely(proc->extra->pthread_tid_offset)) {
            // self->tid currently holds the raw pthread_t value from CPython's
            // thread_id field.  Read the pthread struct to recover the real OS
            // TID.  If this read fails (e.g. the Python process is mid-GC and
            // the thread-state data we sampled is transiently inconsistent),
            // skip this thread rather than proceeding with an unresolved tid
            // that would later crash the interrupted-bitmap access.
            if (fail(read_pthread_t(self->proc, (void*)self->tid)))
                FAIL;
            int o     = proc->extra->pthread_tid_offset;
            self->tid = o > 0 ? proc->extra->_pthread_buffer[o] : (pid_t)((pid_t*)proc->extra->_pthread_buffer)[-o];
            if (self->tid >= max_pid || self->tid == 0) {
                log_e("Invalid TID detected");
                self->tid = 0;
                FAIL;
            }
        }
    }
#endif

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__read_remote(py_thread_t* self, raddr_t addr) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid thread pointer");
        FAIL;
    } // GCOV_EXCL_STOP

    return _py_thread__read_remote(self, addr);
}

// ----------------------------------------------------------------------------
static inline void
_py_thread__load_stack(py_thread_t* self) {
    if (!isvalid(self->stack) && isvalid(self->stack_raddr)) {
        self->stack = stack_chunk_new(self->proc->ref, self->stack_raddr);
    }
}

// ----------------------------------------------------------------------------
// Read the thread state and immediately perform the top-frame cache check.
// Sets self->is_repeat; if not a repeat, loads the stack chunk right away to
// minimise the window between the thread-state read and the frame data read.
int
py_thread__read_with_stack_remote(py_thread_t* self, raddr_t addr, thread_tracker_t* tracker) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid thread pointer");
        FAIL;
    } // GCOV_EXCL_STOP

    if (fail(_py_thread__read_remote(self, addr)))
        FAIL;

    self->is_repeat = false;

    if (!isvalid(tracker))
        SUCCESS;

    thread_tracker_entry_t* entry = thread_tracker__get_or_create(tracker, self->tid);
    if (!isvalid(entry))
        SUCCESS;

    entry->last_gen = tracker->sample_gen;

    V_DESC(self->proc->py_v);
    // For Python < 3.11 heap-allocated frame objects can be freed and
    // reallocated at the same address; verify the code object too.
    // For >= 3.11 frames live in a stack chunk with variable sizing, so
    // address collisions are rare enough that the frame check alone suffices.
    void* top_code = (isvalid(self->top_frame) && V_MAX(3, 10)) ? _py_thread__read_top_code(self) : NULL;

    if (entry->top_frame == self->top_frame) {
        if (V_MIN(3, 11) || top_code == entry->top_code) {
            self->is_repeat = true;
            SUCCESS;
        }
    }

    _py_thread__load_stack(self);

    // Track the last seen top frame information.
    entry->top_frame = self->top_frame;
    entry->top_code  = top_code;

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__next(py_thread_t* self, thread_tracker_t* tracker) {
    V_DESC(self->proc->py_v);

    if (V_MIN(3, 11)) {
        stack_chunk__destroy(self->stack);
        self->stack = NULL;
    }

    if (!isvalid(self->next))
        STOP(ITEREND);

    log_t("Found next thread");

    return isvalid(tracker) ? py_thread__read_with_stack_remote(self, self->next, tracker)
                            : py_thread__read_remote(self, self->next);
}

// ----------------------------------------------------------------------------
void
py_thread__unwind(py_thread_t* self) {
    bool error = false;

    // Platform-specific native stack unwinding dispatch.
    // Each platform header defines _py_thread__unwind_native which handles
    // the is_interrupted check, kernel stack, and native frame unwinding.
    _py_thread__unwind_native(self, &error);

    V_DESC(self->proc->py_v);

    // Clear the Python stack state before any Python stack unwinding.
    stack_reset();

    if (self->is_repeat) {
        stack_push_py_repeat();
    } else if (isvalid(self->top_frame)) {
        // Ensure the stack chunk is loaded before unwinding
        _py_thread__load_stack(self);

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

    if (error)
        stats_count_error();
}

// ----------------------------------------------------------------------------
int
py_thread_allocate(void) {
    if (isvalid(_stack)) // GCOV_EXCL_LINE
        SUCCESS;         // GCOV_EXCL_LINE

    if (fail(stack_allocate(MAX_STACK_SIZE))) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    max_pid = pid_max() + 1;

    if (fail(_py_thread_allocate_native()))
        FAIL;

    SUCCESS;
}

// ----------------------------------------------------------------------------
void
py_thread_free(void) {
#ifdef DEBUG
    if (_stack_chunk_count) {
        log_d(
            "Stack chunk hit ratio: %d/%d (%0.2f%%)\n", _stack_chunk_count - _stack_chunk_misses, _stack_chunk_count,
            (_stack_chunk_count - _stack_chunk_misses) * 100.0 / _stack_chunk_count
        );
    }
#endif

    stack_deallocate();

    _py_thread_free_native();
}
