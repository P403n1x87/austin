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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#include "../argparse.h"
#include "../cache.h"
#include "../events.h"
#include "../hints.h"
#include "../logging.h"
#include "../mem.h"
#include "../py_thread.h"
#include "../resources.h"
#include "../stack.h"

// ---- Platform-specific static variables ------------------------------------

#ifdef AUSTINP
static void** _tids = NULL; // libunwind-ptrace contexts, indexed by kernel TID
#else
// fp-walk mode: track which TIDs have been ptrace-seized (no libunwind context)
static bool* _seized = NULL; // ptrace-seized flag, indexed by kernel TID
#endif
static unsigned char* _tids_idle = NULL; // idle-state bitmap, indexed by kernel TID
static unsigned char* _tids_int  = NULL; // interrupted-state bitmap, indexed by kernel TID
// Compact list of TIDs whose _tids_int bit is currently set.  The bitmap alone
// gives O(1) is_interrupted queries, but resuming every interrupted thread would
// otherwise require scanning the full (~max_pid/8 byte) bitmap each sample.
static pid_t*         _int_list  = NULL;
static size_t         _int_n     = 0;    // number of valid entries in _int_list
static size_t         _int_cap   = 0;    // allocated capacity of _int_list
static char**         _kstacks   = NULL; // kernel stack strings, indexed by kernel TID

// ---- Hot-path inline helpers: idle/interrupted state -----------------------
// These are called per-thread per-sample.  static inline keeps them inlinable
// within the py_thread.c translation unit; extern wrappers in py_thread.c
// provide linkage for cross-TU callers (py_proc.c).

static inline int
_py_thread__set_idle(py_thread_t* self) {
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
static inline int
_py_thread__set_interrupted(py_thread_t* self, bool state) {
    unsigned char bit   = 1 << (self->tid & 7);
    size_t        index = self->tid >> 3;

    if (state) {
        // Idempotent: already interrupted means the TID is already in the list.
        if (_tids_int[index] & bit)
            SUCCESS;

        // Grow the list first.  Setting the bitmap bit before the TID is in
        // the list would leave the bit set but the resume path unable to
        // find it, stranding the thread in ptrace-stop.
        if (_int_n >= _int_cap) {
            size_t new_cap  = _int_cap ? _int_cap * 2 : 16;
            pid_t* new_list = (pid_t*)realloc(_int_list, new_cap * sizeof(pid_t));
            if (!isvalid(new_list)) { // GCOV_EXCL_START
                set_error(MALLOC, "Failed to grow interrupted TID list");
                FAIL;
            } // GCOV_EXCL_STOP
            _int_list = new_list;
            _int_cap  = new_cap;
        }
        _int_list[_int_n++]  = (pid_t)self->tid;
        _tids_int[index]    |= bit;
    } else {
        // Clearing leaves the TID in _int_list; the resume path filters by bit.
        _tids_int[index] &= ~bit;
    }
    SUCCESS;
}

// ----------------------------------------------------------------------------
static inline bool
_py_thread__is_interrupted(py_thread_t* self) {
    return (_tids_int[self->tid >> 3] & (1 << (self->tid & 7))) != 0;
}

// ---- Thread state query ----------------------------------------------------

// ----------------------------------------------------------------------------
bool
py_thread__is_idle(py_thread_t* self) {
    if (pargs_native) {
        size_t index  = self->tid >> 3;
        int    offset = self->tid & 7;

        return _tids_idle[index] & (1 << offset);
    }
    char file_name[64];
    char buffer[2048] = "";

    sprintf(file_name, "/proc/%d/task/%" PRIuPTR "/stat", self->proc->pid, self->tid);

    cu_fd fd = open(file_name, O_RDONLY);
    if (fd == -1) { // GCOV_EXCL_START
        set_error(IO, "Cannot open thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    if (read(fd, buffer, 2047) == 0) { // GCOV_EXCL_START
        set_error(IO, "Cannot read thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    char* p = strchr(buffer, ')'); // GCOV_EXCL_START
    if (!isvalid(p)) {
        set_error(OS, "Invalid thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    p += 2;
    if (*p == ' ')
        p++; // GCOV_EXCL_LINE

    return (*p != 'R');
}

// ---- Thread control (native mode) -----------------------------------------

// Suspend the thread: send PTRACE_INTERRUPT and wait for ptrace-stop.
static inline int
_py_thread__suspend(py_thread_t* self) {
    if (fail(wait_ptrace(PTRACE_INTERRUPT, self->tid, 0, 0)))
        FAIL;

    // Consume the ptrace-stop notification so that the thread is fully
    // stopped before reading its registers or unwinding its stack.
    if (fail(wait_thread_stop(self->tid))) {
        log_d("ptrace: thread %" PRIuPTR " did not stop in time, resuming", self->tid);
        ptrace(PTRACE_CONT, self->tid, 0, 0);
        set_error(OS, "Thread did not stop in time");
        FAIL;
    }
    SUCCESS;
}

// Resume a single thread.
static inline int
_py_thread__resume(py_thread_t* self) {
    if (ptrace(PTRACE_CONT, self->tid, 0, 0)) {
        set_error(OS, "ptrace: failed to resume thread");
        FAIL;
    }
    SUCCESS;
}

// Resume every thread whose interrupted bit is set by iterating the compact
// list of TIDs populated during the interrupt phase, rather than scanning the
// full (~max_pid/8 byte) bitmap.  Entries whose bit has already been cleared
// (e.g. by the TID-reuse path in _py_thread__seize) are filtered out here.
void
py_thread__resume_all_interrupted(void) {
    for (size_t i = 0; i < _int_n; i++) {
        pid_t         tid   = _int_list[i];
        unsigned char bit   = (unsigned char)(1 << (tid & 7));
        size_t        index = (size_t)tid >> 3;

        if (!(_tids_int[index] & bit))
            continue; // bit cleared elsewhere; nothing to resume

        if (ptrace(PTRACE_CONT, tid, 0, 0)) {
            log_d("ptrace: failed to resume thread %d (errno: %d)", tid, errno);
        } else {
            log_t("ptrace: thread %d resumed", tid);
        }
        _tids_int[index] &= ~bit; // always clear so the thread isn't stuck
    }
    _int_n = 0;
}

// ---- Kernel stack capture (Linux only) -------------------------------------

#define MAX_STACK_FILE_SIZE 2048

static inline int
_py_thread__save_kernel_stack(py_thread_t* self) {
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

// ---- Native stack unwinding ------------------------------------------------
// Linux: remote unwinding
//   AUSTINP  — libunwind-ptrace (any arch, full accuracy)
//   plain austin — frame-pointer walk (x86-64 / aarch64)
// ----------------------------------------------------------------------------

#ifdef AUSTINP

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
            cached_string_t* scope = NULL;
            vm_range_t*      range = vm_range_tree__find(self->proc->maps_tree, pc);

#ifdef HAVE_BFD
            if (pargs.where && isvalid(range)) {
                if (range->lo > 0)
                    frame = get_native_frame(range->name, pc - range->lo, frame_key);
            }
#endif
            if (!isvalid(frame)) {
                unw_proc_info_t pi;
                if (success(unw_get_proc_info(&cursor, &pi))) {
                    // Tag the scope key with bit 0 to avoid colliding with
                    // the filename key (which uses the raw PC value). PCs are
                    // at minimum 2-byte aligned on all supported architectures.
                    key_dt scope_key = (key_dt)pi.start_ip | 1;
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
                    // Fallback: resolve via ELF .dynsym/.symtab when libunwind
                    // cannot name the frame (e.g. stripped but exported symbols).
                    if (!isvalid(scope) && isvalid(range)) {
                        if (range->lo > 0) {
                            const char* fname = get_func_name(pc, range->name, range->lo);
                            if (isvalid(fname)) {
                                scope = cached_string_new(scope_key, strdup(fname));
                                if (!isvalid(scope))
                                    FAIL; // GCOV_EXCL_LINE
                                lru_cache__store(string_cache, scope_key, (value_t)scope);
                                event_handler__emit_new_string(scope);
                            }
                        }
                    }
                }
                if (!isvalid(scope)) {
                    scope  = UNKNOWN_SCOPE;
                    offset = 0;
                }

                // Resolve filename from vm_range_tree — populated when pargs_native.
                // Falls back to "native@<pc>" when the range is unknown.
                key_dt           filename_key = (key_dt)pc;
                cached_string_t* filename     = lru_cache__maybe_hit(string_cache, filename_key);
                if (!isvalid(filename)) {
                    if (isvalid(range))
                        snprintf(_native_buf, MAXLEN, "%s", range->name);
                    else
                        snprintf(_native_buf, MAXLEN, "native@%" PRIxPTR, pc);
                    filename = cached_string_new(filename_key, strdup(_native_buf));
                    if (!isvalid(filename)) {
                        FAIL; // GCOV_EXCL_LINE
                    }
                    lru_cache__store(string_cache, filename_key, (value_t)filename);
                    event_handler__emit_new_string(filename);
                }

                frame = frame_new(frame_key, filename, scope, offset, 0, 0, 0);
                if (!isvalid(frame)) // GCOV_EXCL_LINE
                    FAIL;            // GCOV_EXCL_LINE
            }

            lru_cache__store(cache, frame_key, (value_t)frame);

            event_handler__emit_new_frame(frame);
        }

        if (unlikely(is_pyeval_frame(frame->scope))) {
            stack_native_push((frame_t*)EVAL_FRAME_MAGIC);
            if (self->is_repeat)
                break;
        } else
            stack_native_push(frame);
    } while (!stack_native_full() && unw_step(&cursor) > 0);

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack (AUSTINP/libunwind) */

// AUSTINP: ptrace-based thread seize (creates libunwind context)
int
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

#elif defined(__x86_64__) || defined(__aarch64__)
/* !AUSTINP: frame-pointer walk on x86-64 / aarch64 */

#include <elf.h>     // NT_PRSTATUS
#include <sys/uio.h> // struct iovec, process_vm_readv
#if defined(__x86_64__)
#include <sys/user.h> // struct user_regs_struct
#elif defined(__aarch64__)
// struct user_pt_regs is defined in <asm/ptrace.h> on glibc, but musl doesn't
// ship kernel headers.  The layout is stable kernel ABI so we define it
// ourselves to avoid the dependency.
struct user_pt_regs {
    unsigned long long regs[31];
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};
#endif

#include "unwind.h"

#if defined(__aarch64__)
// Mask off pointer-authentication bits; user VAs on aarch64 Linux are <= 48 bits.
#define _STRIP_PAC(addr) ((uintptr_t)(addr) & 0x0000ffffffffffffull)
#else
#define _STRIP_PAC(addr) ((uintptr_t)(addr))
#endif

// Capture PC, frame-pointer, and stack-pointer for a ptrace-stopped thread.
static inline int
_capture_regs(pid_t tid, uintptr_t* pc_out, uintptr_t* fp_out, uintptr_t* sp_out) {
#if defined(__x86_64__)
    struct user_regs_struct regs;
    struct iovec            iov = {.iov_base = &regs, .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_PRSTATUS, &iov) < 0)
        FAIL;
    *pc_out = (uintptr_t)regs.rip;
    *fp_out = (uintptr_t)regs.rbp;
    *sp_out = (uintptr_t)regs.rsp;
#elif defined(__aarch64__)
    struct user_pt_regs regs;
    struct iovec        iov = {.iov_base = &regs, .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_GETREGSET, tid, (void*)(uintptr_t)NT_PRSTATUS, &iov) < 0)
        FAIL;
    *pc_out = (uintptr_t)regs.pc;
    *fp_out = (uintptr_t)regs.regs[29]; // x29 is the frame pointer on aarch64
    *sp_out = (uintptr_t)regs.sp;
#endif
    SUCCESS;
}

// Walk the native call stack using the frame-pointer chain.
// Requires the thread to already be in ptrace-stop (via PTRACE_INTERRUPT).
// Prefetches one 4 KB page at the top of the stack (covers the common case
// where consecutive frames are nearby), then falls back to exact 16-byte reads
// when FP moves outside that page — smaller reads are faster with process_vm_readv.
// Filenames are resolved from vm_range_tree; function names are resolved via
// get_func_name() — both are cached, so the first occurrence of each
// unique PC pays the lookup cost; all subsequent samples are cache hits.
static inline int
_py_thread__unwind_native_frame_stack(py_thread_t* self) {
    stack_native_reset();

    uintptr_t pc = 0, fp = 0, sp = 0;
    if (fail(_capture_regs((pid_t)self->tid, &pc, &fp, &sp))) {
        set_error(OS, "Failed to read thread registers via PTRACE_GETREGSET");
        FAIL;
    }

    // Once we start using CFI we must not revert to fp-walk: the fp recovered
    // by cfi_step is the saved RBP (a callee-saved register), not necessarily
    // a frame-pointer record.  Mixing modes causes spurious / repeated frames.
#if defined(__x86_64__)
    // On x86-64, most binaries are compiled without frame pointers
    // (-fomit-frame-pointer is the default at -O2).  RBP is a general-purpose
    // register whose value may look like a valid stack address but isn't a
    // frame-pointer record.  Always use CFI.  We keep fp intact because
    // cfi_step needs it when the CFA rule says CFA = RBP + offset.
    bool use_cfi = true;
#elif defined(__aarch64__)
    // On aarch64, the AAPCS mandates frame pointers (x29/x30 pairs on the
    // stack).  fp-walk is the preferred fast path.
    if (fp < sp || fp >> 48)
        fp = 0;
    bool use_cfi = (fp == 0);
#endif

    lru_cache_t* cache        = self->proc->frame_cache;
    lru_cache_t* string_cache = self->proc->string_cache;

    // Prefetch the page containing the top of the stack. Most frames are likely
    // within this page, so one read amortises the cost across the whole walk.
#define _STACK_BUF_SIZE 4096
    uint8_t   _stack_buf[_STACK_BUF_SIZE];
    uintptr_t _stack_buf_base = sp & ~((uintptr_t)(_STACK_BUF_SIZE - 1));
    {
        struct iovec local  = {.iov_base = _stack_buf, .iov_len = _STACK_BUF_SIZE};
        struct iovec remote = {.iov_base = (void*)_stack_buf_base, .iov_len = _STACK_BUF_SIZE};
        if (process_vm_readv(self->proc->pid, &local, 1, &remote, 1, 0) != _STACK_BUF_SIZE)
            _stack_buf_base = 0; // prefetch failed; fall through to per-frame reads
    }

    while (!stack_native_full() && pc != 0) {
        key_dt   frame_key = (key_dt)pc;
        frame_t* frame     = lru_cache__maybe_hit(cache, frame_key);

        if (!isvalid(frame)) {
            // Resolve filename from the vm_range_tree (populated when pargs_native).
            // O(log n) tree lookup; result cached in string_cache so paid once per PC.
            vm_range_t* range = vm_range_tree__find(self->proc->maps_tree, pc);

            // If the PC doesn't fall in any mapped region it is a spurious
            // return address (e.g. the kernel-set return address above _start
            // or clone).  Stop unwinding instead of emitting a bogus frame.
            if (!isvalid(range))
                break;

            key_dt           filename_key = (key_dt)pc;
            cached_string_t* filename     = lru_cache__maybe_hit(string_cache, filename_key);
            if (!isvalid(filename)) {
                snprintf(_native_buf, MAXLEN, "%s", range->name);
                filename = cached_string_new(filename_key, strdup(_native_buf));
                if (!isvalid(filename))
                    FAIL;
                lru_cache__store(string_cache, filename_key, (value_t)filename);
                event_handler__emit_new_string(filename);
            }

            // Resolve function name via ELF symbol table.
            // The symbol table is loaded once per binary and cached; lookups are
            // O(log n) binary search. After the first hit for each PC, free.
            key_dt           scope_key = frame_key + 1;
            cached_string_t* scope     = lru_cache__maybe_hit(string_cache, scope_key);
            if (!isvalid(scope)) {
                const char* fname = NULL;
                if (isvalid(range)) {
                    if (range->lo > 0)
                        fname = get_func_name(pc, range->name, range->lo);
                }
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

        if (unlikely(is_pyeval_frame(frame->scope))) {
            stack_native_push((frame_t*)EVAL_FRAME_MAGIC);
            if (self->is_repeat)
                break;
        } else
            stack_native_push(frame);

        if (use_cfi) {
            // CFI mode: use .eh_frame unwinding.  fp still holds the current
            // RBP value and is passed to cfi_step for CFA = RBP+N rules.
            // Pass the prefetched stack buffer so cfi_step can serve CFA reads
            // directly from it, skipping the process_vm_readv syscall when the
            // return address falls within the prefetched page.
            if (!cfi_step(
                    self->proc->pid, self->proc->maps_tree, &pc, &sp, &fp, _stack_buf_base ? _stack_buf : NULL,
                    _stack_buf_base, _STACK_BUF_SIZE
                ))
                break;
            pc = _STRIP_PAC(pc);
            // Do NOT clear _stack_buf_base here: CFA grows monotonically up the
            // stack so subsequent steps are likely to land in the same page.
            continue;
        }

        // Read the next frame record: [fp] = saved_fp, [fp+8] = return_addr.
        // Serve from the prefetched page when FP falls within it; otherwise
        // read exactly 16 bytes — smaller out-of-buffer reads are faster.
        uintptr_t frame_data[2] = {0, 0};
        if (_stack_buf_base != 0 && fp >= _stack_buf_base
            && fp - _stack_buf_base + sizeof(frame_data) <= _STACK_BUF_SIZE) {
            memcpy(frame_data, _stack_buf + (fp - _stack_buf_base), sizeof(frame_data));
        } else {
            struct iovec local  = {.iov_base = frame_data, .iov_len = sizeof(frame_data)};
            struct iovec remote = {.iov_base = (void*)fp, .iov_len = sizeof(frame_data)};
            if (process_vm_readv(self->proc->pid, &local, 1, &remote, 1, 0) != (ssize_t)sizeof(frame_data)) {
                // process_vm_readv failed — fp may be garbage; switch to CFI.
                use_cfi = true;
                if (!cfi_step(
                        self->proc->pid, self->proc->maps_tree, &pc, &sp, &fp, _stack_buf_base ? _stack_buf : NULL,
                        _stack_buf_base, _STACK_BUF_SIZE
                    ))
                    break;
                pc              = _STRIP_PAC(pc);
                _stack_buf_base = 0; // fp-walk state was invalid; buffer may not be trustworthy
                continue;
            }
        }

        uintptr_t new_fp = frame_data[0];
        uintptr_t new_pc = _STRIP_PAC(frame_data[1]);

        // If fp didn't advance (or went backwards) the chain is corrupt;
        // switch to CFI before we loop forever.
        if (new_fp != 0 && new_fp <= fp) {
            use_cfi = true;
            if (!cfi_step(
                    self->proc->pid, self->proc->maps_tree, &pc, &sp, &fp, _stack_buf_base ? _stack_buf : NULL,
                    _stack_buf_base, _STACK_BUF_SIZE
                ))
                break;
            pc              = _STRIP_PAC(pc);
            _stack_buf_base = 0; // fp-walk state was invalid; buffer may not be trustworthy
            continue;
        }

        fp = new_fp;
        pc = new_pc;
    }
#undef _STACK_BUF_SIZE

    SUCCESS;
} /* _py_thread__unwind_native_frame_stack (fp-walk) */

// fp-walk: ptrace-seize (no libunwind context — just seize and track)
int
_py_thread__seize(py_thread_t* self) {
    if (_seized[self->tid]) {
        // Already seized. Check for TID reuse: if the original thread exited
        // and a new thread took the same TID, our ptrace attachment is stale.
        char task_path[48];
        sprintf(task_path, "/proc/%d/task/%" PRIuPTR, self->proc->pid, self->tid);
        if (access(task_path, F_OK) == 0)
            SUCCESS; // Same thread still alive

        // Thread exited — release the stale attachment and re-seize.
        log_d("ptrace: TID %" PRIuPTR " reused, detaching stale attachment", self->tid);
        if (fail(wait_ptrace(PTRACE_DETACH, (pid_t)self->tid, 0, 0)))
            log_d("ptrace: failed to detach stale TID %" PRIuPTR, self->tid);
        _seized[self->tid]          = false;
        _tids_int[self->tid >> 3]  &= ~(1 << (self->tid & 7));
        _tids_idle[self->tid >> 3] &= ~(1 << (self->tid & 7));
    }

    if (fail(wait_ptrace_seize((pid_t)self->tid))) { // GCOV_EXCL_START
        set_error(OS, "Failed to seize thread");
        FAIL;
    } // GCOV_EXCL_STOP

    log_d("ptrace: thread %" PRIuPTR " seized (fp-walk)", self->tid);
    _seized[self->tid] = true;

    SUCCESS;
}

#else
/* Unsupported architecture for native mode (e.g. armv7, ppc64le).
   Provide a no-op seize so the code compiles; native mode is gated
   at runtime by pargs_native which is never set on these archs. */
int
_py_thread__seize(py_thread_t* self) {
    (void)self;
    SUCCESS;
}

#endif /* AUSTINP / fp-walk / unsupported arch */

// ---- Per-thread interrupt (hot path) ----------------------------------------
// Seize, capture idle state, suspend, and mark as interrupted.
// Called once per thread per sample from _py_proc__interrupt_threads.

int
py_thread__interrupt(py_thread_t* self) {
    if (fail(_py_thread__seize(self)))
        FAIL;

#ifdef AUSTINP
    if (pargs.kernel && fail(_py_thread__save_kernel_stack(self)))
        FAIL;
#endif

    if (fail(_py_thread__set_idle(self)))
        FAIL;

    if (fail(_py_thread__suspend(self)))
        FAIL;

    if (fail(_py_thread__set_interrupted(self, true))) {
        _py_thread__resume(self);
        FAIL;
    }

    log_t("linux: thread %ld suspended", (long)self->tid);
    SUCCESS;
}

// ---- Native unwind dispatch ------------------------------------------------
// Called from py_thread__unwind to handle all native-mode stack unwinding.

static inline void
_py_thread__unwind_native(py_thread_t* self, bool* error) {
    // Only unwind the native stack if this thread was stopped during the
    // interrupt phase. A thread that appears in the Python linked list but
    // was NOT interrupted (i.e. it was created after we finished interrupting)
    // is still running — unwinding its registers would produce garbage or
    // crash the unwinder.
    if (!_py_thread__is_interrupted(self))
        return;

#ifdef AUSTINP
    // We sample the kernel frame stack BEFORE interrupting because
    // otherwise we would see the ptrace syscall call stack, which is not
    // very interesting. The downside is that the kernel stack might not be
    // in sync with the other ones.
    if (pargs.kernel) {
        _py_thread__unwind_kernel_frame_stack(self);
    }
    // AUSTINP: native sampling is always active (controlled by pargs_native
    // in _py_proc__interrupt_threads — if we reach here, it was requested).
    if (fail(_py_thread__unwind_native_frame_stack(self))) {
        *error = true;
    }
#elif defined(__x86_64__) || defined(__aarch64__)
    // fp-walk: only unwind when native mode is explicitly enabled.
    if (fail(_py_thread__unwind_native_frame_stack(self))) {
        *error = true;
    }
#endif
}

// ---- Allocation/deallocation -----------------------------------------------

static int
_py_thread_allocate_native(void) {
#ifdef AUSTINP
    _tids = (void**)calloc(max_pid, sizeof(void*));
    if (!isvalid(_tids)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate thread context buffer");
        goto failed;
    } // GCOV_EXCL_STOP
#else
    _seized = (bool*)calloc(max_pid, sizeof(bool));
    if (!isvalid(_seized)) { // GCOV_EXCL_START
        set_error(MALLOC, "Failed to allocate thread seized buffer");
        goto failed;
    } // GCOV_EXCL_STOP
#endif

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

#ifdef AUSTINP
    if (pargs.kernel) {
        _kstacks = (char**)calloc(max_pid, sizeof(char*));
        if (!isvalid(_kstacks)) { // GCOV_EXCL_START
            set_error(MALLOC, "Failed to allocate kernel stack buffer");
            goto failed;
        } // GCOV_EXCL_STOP
    }
#endif /* AUSTINP */
    SUCCESS;

failed: // GCOV_EXCL_START
#ifdef AUSTINP
    sfree(_tids);
#else
    sfree(_seized);
#endif
    sfree(_tids_idle);
    sfree(_tids_int);
    sfree(_kstacks);

    FAIL; // GCOV_EXCL_STOP
}

static void
_py_thread_free_native(void) {
    for (pid_t tid = 0; tid < (pid_t)max_pid; tid++) {
#ifdef AUSTINP
        if (isvalid(_tids[tid])) {
            _UPT_destroy(_tids[tid]);
            if (fail(wait_ptrace(PTRACE_DETACH, tid, 0, 0)))
                log_d("ptrace: failed to detach thread %ld", tid);
            else
                log_d("ptrace: thread %ld detached", tid);
        }
#else
        if (_seized[tid]) {
            if (fail(wait_ptrace(PTRACE_DETACH, tid, 0, 0)))
                log_d("ptrace: failed to detach thread %ld", tid);
            else
                log_d("ptrace: thread %ld detached", tid);
        }
#endif
        if (isvalid(_kstacks) && isvalid(_kstacks[tid]))
            sfree(_kstacks[tid]);
    }
#ifdef AUSTINP
    sfree(_tids);
#else
    sfree(_seized);
#if defined(__x86_64__) || defined(__aarch64__)
    cfi_cache_destroy();
#endif
#endif
    sfree(_tids_idle);
    sfree(_tids_int);
    sfree(_int_list);
    _int_n = _int_cap = 0;
    sfree(_kstacks);
}
