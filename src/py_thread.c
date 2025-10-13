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
#ifdef NATIVE
static void**         _tids      = NULL;
static unsigned char* _tids_idle = NULL;
static unsigned char* _tids_int  = NULL;
static char**         _kstacks   = NULL;
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
// ----------------------------------------------------------------------------
int
py_thread__set_idle(py_thread_t* self) {
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

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__set_interrupted(py_thread_t* self, bool state) {
    unsigned char bit   = 1 << (self->tid & 7);
    size_t        index = self->tid >> 3;

    if (state) {
        _tids_int[index] |= bit;
    } else {
        _tids_int[index] &= ~bit;
    }

    SUCCESS;
}

// ----------------------------------------------------------------------------
int
py_thread__is_interrupted(py_thread_t* self) {
    return _tids_int[self->tid >> 3] & (1 << (self->tid & 7));
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

// ----------------------------------------------------------------------------
static char _native_buf[MAXLEN];

static inline int
wait_unw_init_remote(unw_cursor_t* c, unw_addr_space_t as, void* arg) {
    int            outcome = 0;
    microseconds_t end     = gettime() + 1000;
    while (gettime() <= end && (outcome = unw_init_remote(c, as, arg)) == -UNW_EBADREG)
        sched_yield();
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
static inline int
_py_thread__seize(py_thread_t* self) {
    // TODO: If a TID is reused we will never seize it!
    if (!isvalid(_tids[self->tid])) {
        if (fail(wait_ptrace(PTRACE_SEIZE, self->tid, 0, 0))) { // GCOV_EXCL_START
            set_error(OS, "Failed to seize thread");
            FAIL;
        } else { // GCOV_EXCL_STOP
            log_d("ptrace: thread %d seized", self->tid);
        }
        _tids[self->tid] = _UPT_create(self->tid);
        if (!isvalid(_tids[self->tid])) { // GCOV_EXCL_START
            set_error(OS, "Failed to create libunwind context");
            FAIL;
        } // GCOV_EXCL_STOP
    }
    SUCCESS;
}

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

    // We sample the kernel frame stack BEFORE interrupting because otherwise
    // we would see the ptrace syscall call stack, which is not very interesting.
    // The downside is that the kernel stack might not be in sync with the other
    // ones.
    if (pargs.kernel) {
        _py_thread__unwind_kernel_frame_stack(self);
    }
    if (fail(_py_thread__unwind_native_frame_stack(self))) {
        error = true;
    }

    // Update the thread state to improve guarantees that it will be in sync with
    // the native stack just collected
    py_thread__read_remote(self, self->addr);
#endif
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

#ifdef NATIVE
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
#endif /* NATIVE */

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

#ifdef NATIVE
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
#endif
}
