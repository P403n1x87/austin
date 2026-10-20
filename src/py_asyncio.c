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

#include "py_asyncio.h"

#include "events.h"
#include "logging.h"
#include "mem.h"
#include "py_set.h"
#include "py_string.h"
#include "py_thread.h"
#include "task_tracker.h"

// AsyncioDebug carries no cookie, so validate by sanity-checking the
// embedded sizes instead -- never hardcoded, since they vary (e.g.
// free-threaded builds add a task_tid field to TaskObj).
#define IS_SANE_SIZE(x) ((x) > 0 && (x) <= 4096)

// sizeof(PyInterpreterState) runs into the hundreds of KB on modern builds,
// so it needs a much larger bound than IS_SANE_SIZE.
#define IS_SANE_INTERP_SIZE(x) ((x) > 0 && (x) <= (1 << 20))

// sizeof(PyThreadState); free-threaded builds embed substantial extra
// per-thread state, so this also needs a larger bound than IS_SANE_SIZE.
#define IS_SANE_THREAD_SIZE(x) ((x) > 0 && (x) <= (1 << 16))

// AsyncioDebug equivalent of copy_field_v (mem.h). group is one of
// task_object/interpreter_state/thread_state (the asyncio_ prefix they
// share is added here); raddr is the object's own address, not the field's.
#define copy_asyncio_field(self, group, field, raddr, dst)                                                 \
    copy_memory((self)->ref, (raddr) + (self)->asyncio_offsets.asyncio_##group.field, sizeof(dst), &(dst))

static bool
_py_asyncio__validate(Py_AsyncioModuleDebugOffsets* offsets) {
    return IS_SANE_SIZE(offsets->asyncio_task_object.size)
        && IS_SANE_INTERP_SIZE(offsets->asyncio_interpreter_state.size)
        && IS_SANE_THREAD_SIZE(offsets->asyncio_thread_state.size);
} // _py_asyncio__validate

// ----------------------------------------------------------------------------
void
py_asyncio__validate_and_cache(py_proc_t* self) {
    // Read straight into the destination -- consumers all gate on
    // asyncio_debug_found first, so a failed/partial read here is harmless.
    if (fail(copy_remote(self->ref, self->map.asyncio_debug.base, self->asyncio_offsets))) { // GCOV_EXCL_START
        log_d("Cannot read AsyncioDebug offsets from remote process");
        return;
    } // GCOV_EXCL_STOP

    if (!_py_asyncio__validate(&self->asyncio_offsets)) { // GCOV_EXCL_START
        log_d("AsyncioDebug offsets failed sanity check; discarding");
        self->map.asyncio_debug.base = NULL; // Retry from scratch next time.
        return;
    } // GCOV_EXCL_STOP

    self->asyncio_debug_found = true;

    log_d(
        "AsyncioDebug offsets found: task_object.size=%llu (name=%llu, awaited_by=%llu, coro=%llu, "
        "node=%llu), thread_state.size=%llu (running_loop=%llu, running_task=%llu)",
        (unsigned long long)self->asyncio_offsets.asyncio_task_object.size,
        (unsigned long long)self->asyncio_offsets.asyncio_task_object.task_name,
        (unsigned long long)self->asyncio_offsets.asyncio_task_object.task_awaited_by,
        (unsigned long long)self->asyncio_offsets.asyncio_task_object.task_coro,
        (unsigned long long)self->asyncio_offsets.asyncio_task_object.task_node,
        (unsigned long long)self->asyncio_offsets.asyncio_thread_state.size,
        (unsigned long long)self->asyncio_offsets.asyncio_thread_state.asyncio_running_loop,
        (unsigned long long)self->asyncio_offsets.asyncio_thread_state.asyncio_running_task
    );
} // py_asyncio__validate_and_cache

// ----------------------------------------------------------------------------
// Phase 2: task-graph walk (waiter DAG + suspended-task coroutine stacks).
// See py_asyncio.h for the call-site contract.
//
// Bounds below cap iteration counts rather than trust remote data to be
// well-formed, guarding against corrupted memory or a task graph the target
// process is concurrently mutating.

// Matches CPython's MAX_ITERATIONS safety valve for the intrusive task list.
#define MAX_TASK_LIST_ITER (2 << 15)

static inline bool
_is_plausible_ptr(raddr_t p) {
    return isvalid(p) && ((uintptr_t)p % sizeof(void*)) == 0;
}

// ----------------------------------------------------------------------------
// Task identity computation to avoid address-reuse collisions.
static inline uintptr_t
_py_asyncio__entry_id(task_tracker_entry_t* entry) {
    return entry->id;
} // _py_asyncio__entry_id

// ----------------------------------------------------------------------------
// Close a task that is going away: a report with empty frames, the only
// signal readers get that it is gone -- for every task readers know of, i.e.
// that was reported. No remote reads needed, so this is safe even if the
// TaskObj has since been freed.
static void
_py_asyncio__flush_task(task_tracker_entry_t* entry) {
    if (!entry->reported)
        return;

    // No name: readers already know it from the task's earlier reports, and
    // the cached name_key may have been redefined by now (see
    // _py_asyncio__resolve_task_name), since the str it was resolved from
    // is likely gone along with the task.
    task_stack_reset();
    event_handler__emit_task_stack_begin(_py_asyncio__entry_id(entry), 0);
    event_handler__emit_task_stack_end();
} // _py_asyncio__flush_task

// ----------------------------------------------------------------------------
// Wire identity of the task (or future) at addr, e.g. a waiter. 0 if it can't
// be tracked, which readers see as a task they know nothing about.
static uintptr_t
_py_asyncio__task_identity(py_proc_t* self, raddr_t addr) {
    if (!_is_plausible_ptr(addr))
        return 0;

    task_tracker_entry_t* entry = task_tracker__get_or_create(self->task_tracker, (uintptr_t)addr);
    if (isvalid(entry))
        return _py_asyncio__entry_id(entry);

    return 0; // GCOV_EXCL_LINE
} // _py_asyncio__task_identity

// ----------------------------------------------------------------------------
static void
_py_asyncio__emit_waiter_set(py_proc_t* self, raddr_t task_addr, uintptr_t task_id, raddr_t set_addr) {
    py_set_t set = {0};
    if (!py_set__read(self->ref, set_addr, self->py_v, &set)) { // GCOV_EXCL_START
        log_d("Cannot read waiter set for task at %p", task_addr);
        return;
    } // GCOV_EXCL_STOP

    if (!py_set__is_valid(&set)) { // GCOV_EXCL_START
        log_d("Invalid waiter set for task at %p (corrupted remote memory); skipping", task_addr);
        return;
    } // GCOV_EXCL_STOP

    py_set_iter_t it = {0};
    raddr_t       key;
    size_t        emitted = 0;
    while (py_set__next(self->ref, &set, &it, &key)) {
        // A waiter that can't be tracked has no id to refer to. It mustn't go
        // out as 0 either, which would mean the task has no waiters at all.
        uintptr_t waiter_id = _py_asyncio__task_identity(self, key);
        if (waiter_id == 0) // GCOV_EXCL_LINE
            continue;       // GCOV_EXCL_LINE
        event_handler__emit_task_waiter(task_id, waiter_id);
        emitted++;
    }

    if (it.seen < set.used)
        log_d("Torn read while walking waiter set for task at %p", task_addr);
    // asyncio keeps the set once a task has had several waiters, so it can
    // also be empty: the task has no waiters, as when there is no set at all.
    else if (emitted == 0)
        event_handler__emit_task_waiter(task_id, 0);
} // _py_asyncio__emit_waiter_set

// ----------------------------------------------------------------------------
// Cheap fingerprint of task_awaited_by -- the waiter itself in the
// single-waiter case, or a fold of the set's table/mask/used triple
// otherwise. Any add/remove/replace changes it.
static uintptr_t
_py_asyncio__waiter_fingerprint(py_proc_t* self, raddr_t awaited_by, bool is_set) {
    if (!isvalid(awaited_by))
        return 0;

    if (!is_set)
        return (uintptr_t)awaited_by | 1; // tag to disambiguate from a set fingerprint of 0

    py_set_t set = {0};
    if (!py_set__read(self->ref, awaited_by, self->py_v, &set)) // GCOV_EXCL_LINE
        return 0;                                               // GCOV_EXCL_LINE

    return ((uintptr_t)set.table << 2) ^ ((uintptr_t)set.mask << 1) ^ (uintptr_t)set.used;
} // _py_asyncio__waiter_fingerprint

// ----------------------------------------------------------------------------
// Resolve a task's name (TaskObj.task_name, a plain str -- no
// threading._active-style indirection needed) into entry->name_key. Reuses
// string_cache and emit_new_string like code.h's filename/scope caching.
//
// The cache is keyed by the str's remote address, which is only safe for as
// long as the str is alive. That holds for the name a task still refers to,
// so a name object already resolved for this entry is reused as is. A new one
// may sit at the address of a since-freed name of another task though --
// routine for short-lived tasks -- so its contents are checked against the
// cached ones, and the key is redefined on a mismatch rather than reporting
// the old task's name.
//
// On failure, the last known-good name is kept rather than clobbered with 0,
// so a transient failure doesn't blank out an otherwise perfectly good name.
static void
_py_asyncio__resolve_task_name(py_proc_t* self, raddr_t task_addr, task_tracker_entry_t* entry) {
    V_DESC(self->py_v);

    raddr_t name_addr = NULL;
    if (fail(copy_asyncio_field(self, task_object, task_name, task_addr, name_addr))
        || !_is_plausible_ptr(name_addr)) // GCOV_EXCL_LINE
        return;                           // GCOV_EXCL_LINE

    if (entry->name_key != 0 && entry->name_addr == (uintptr_t)name_addr)
        return;

    char* name_value = _string_remote(self->ref, name_addr, py_v);
    if (!isvalid(name_value))
        return;

    key_dt           string_key = ptr_key(name_addr);
    cached_string_t* cached     = (cached_string_t*)lru_cache__maybe_hit(self->string_cache, string_key);
    if (isvalid(cached)) {
        if (strcmp(cached->value, name_value) == 0) {
            free(name_value);
        } else {
            free(cached->value);
            cached->value = name_value;
            event_handler__emit_new_string(cached);
        }
    } else {
        cached_string_t* new_string = cached_string_new(string_key, name_value);
        if (!isvalid(new_string)) { // GCOV_EXCL_START
            free(name_value);
            return;
        } // GCOV_EXCL_STOP

        lru_cache__store(self->string_cache, string_key, new_string);
        event_handler__emit_new_string(new_string);
    }

    entry->name_key  = (uintptr_t)string_key;
    entry->name_addr = (uintptr_t)name_addr;
} // _py_asyncio__resolve_task_name

// _PyFrameState's numbering isn't stable across minor versions -- see the
// FRAME_*_3_14/FRAME_*_3_15 constants in python/iframe.h -- hence these
// helpers keyed off py_v->minor rather than a single hardcoded constant.
static inline bool
_py_asyncio__frame_state_done(int minor, int8_t frame_state) {
    // 3.14: FRAME_STATE_FINISHED(S) is (S) >= FRAME_COMPLETED.
    // 3.15+: only FRAME_CLEARED means "don't touch this frame again".
    return minor <= 14 ? frame_state >= FRAME_COMPLETED_3_14 : frame_state == FRAME_CLEARED_3_15;
}

static inline bool
_py_asyncio__frame_state_yield_from(int minor, int8_t frame_state) {
    return minor <= 14 ? frame_state == FRAME_SUSPENDED_YIELD_FROM_3_14
                       : (frame_state == FRAME_SUSPENDED_YIELD_FROM_3_15
                          || frame_state == FRAME_SUSPENDED_YIELD_FROM_LOCKED_3_15);
}

// Safety valve against a corrupted or cyclic await chain -- a real chain is
// only a handful of frames deep.
#define MAX_CORO_CHAIN_DEPTH 64

// _PyStackRef tagged-pointer scheme (stable since its introduction in 3.13):
// the low 2 bits carry tag info, the real address is the rest.
#define UNTAG_STACKREF(bits) ((raddr_t)((uintptr_t)(bits) & ~(uintptr_t)0x3))

// ----------------------------------------------------------------------------
// Reconstructs a suspended task's await chain -- the coroutines it awaits
// via plain `await expr` (not a separate Task, which has its own task-graph
// entry). Can't use the frame's `previous` pointer here (CPython clears it
// on suspend); instead mirrors CPython's own parse_coro_chain /
// handle_yield_from_frame (Modules/_remote_debugging/asyncio.c), reading
// what a suspended coroutine awaits off its own evaluation stack:
//
//   1. Read gi_frame_state; stop if FRAME_CLEARED.
//   2. If SUSPENDED_YIELD_FROM, peek one or two stack slots below the top
//      (see slots_back below) for what it's awaiting, untag it, and recurse
//      only if its type still matches this coroutine's own -- i.e. it's
//      still a plain coroutine, not yet a Task/Future/other awaitable. That
//      type check is what stops the walk at the task-level boundary.
//   3. Push this coroutine's own frame identity (code, lasti) *after*
//      recursing, so the leaf ends up pushed first and the root last --
//      matching the leaf-first order every other unwind path in Austin
//      relies on for its LIFO pop to emit root-first/leaf-last on the wire.
//
// Pushes via stack_py_push directly rather than py_thread__append_iframe_stack,
// which walks the WHOLE previous chain -- right for a normal unwind, wrong
// here: a coroutine currently *executing* still has a live previous into the
// thread's own eval-loop frames, and walking it would garble this
// coroutine's chain together with the thread's own.
//
// Writes the deepest pushed hop into *out_leaf every time, so a partial
// (depth-limited or torn) walk still leaves something usable.
//
// *io_chain_fp folds in every hop's (code, lasti) as the recursion descends,
// covering root to leaf. Frame address is deliberately excluded -- CPython's
// allocator can reuse a just-freed slot, but (code, lasti) is intrinsic to
// the callee and stable per call site, which is what lets two different
// callers of the same shared coroutine (e.g. two asyncio.sleep() call sites)
// fingerprint differently (see task_frame_id_t in task_tracker.h).
static void
_py_asyncio__unwind_coro_chain(
    py_proc_t* self, raddr_t coro_addr, int depth, task_frame_id_t* out_leaf, uint64_t* io_chain_fp
);

// Recurse into what a SUSPENDED_YIELD_FROM coroutine is awaiting, if it's
// still a plain coroutine (its type matches gen_type_addr, the awaiting
// coroutine's own type) -- that comparison is what stops the walk at a
// Task/Future boundary. Guard-clause style since each step depends on the
// last succeeding.
static void
_py_asyncio__recurse_into_awaited(
    py_proc_t* self, raddr_t iframe_addr, raddr_t gen_type_addr, int depth, task_frame_id_t* out_leaf,
    uint64_t* io_chain_fp
) {
    V_DESC(self->py_v);

    uintptr_t stackpointer = 0;
    if (fail(copy_field_v(self->ref, iframe, stackpointer, iframe_addr, stackpointer))
        || stackpointer == 0) // GCOV_EXCL_LINE
        return;               // GCOV_EXCL_LINE

    // The awaited object sits one stack slot below the top on 3.14, but two
    // slots below on 3.15.
    uintptr_t slots_back = V_MAX(3, 14) ? 1 : 2;

    uintptr_t awaited_raw = 0;
    if (fail(
            copy_remote(self->ref, (raddr_t)(stackpointer - slots_back * sizeof(void*)), awaited_raw)
        ))      // GCOV_EXCL_LINE
        return; // GCOV_EXCL_LINE

    raddr_t awaited_addr = UNTAG_STACKREF(awaited_raw);
    if (!_is_plausible_ptr(awaited_addr)) // GCOV_EXCL_LINE
        return;                           // GCOV_EXCL_LINE

    raddr_t awaited_type_addr = NULL;
    if (fail(copy_remote(self->ref, (char*)awaited_addr + py_v->py_object_o_type, awaited_type_addr))
        || awaited_type_addr != gen_type_addr)
        return;

    _py_asyncio__unwind_coro_chain(self, awaited_addr, depth + 1, out_leaf, io_chain_fp);
} // _py_asyncio__recurse_into_awaited

static void
_py_asyncio__unwind_coro_chain(
    py_proc_t* self, raddr_t coro_addr, int depth, task_frame_id_t* out_leaf, uint64_t* io_chain_fp
) {
    if (depth >= MAX_CORO_CHAIN_DEPTH || !_is_plausible_ptr(coro_addr)) // GCOV_EXCL_LINE
        return;                                                         // GCOV_EXCL_LINE

    V_DESC(self->py_v);

    int8_t frame_state = 0;
    if (fail(copy_field_v(self->ref, gen, gi_frame_state, coro_addr, frame_state)) // GCOV_EXCL_START
        || _py_asyncio__frame_state_done(py_v->minor, frame_state))
        return;
    // GCOV_EXCL_STOP

    raddr_t gen_type_addr = NULL;
    if (fail(copy_remote(self->ref, (char*)coro_addr + py_v->py_object_o_type, gen_type_addr))) // GCOV_EXCL_LINE
        return;                                                                                 // GCOV_EXCL_LINE

    raddr_t iframe_addr = (raddr_t)((char*)coro_addr + py_v->py_gen.o_gi_iframe);

    raddr_t code_addr = NULL;
    raddr_t instr_ptr = NULL;
    if (fail(copy_field_v(self->ref, iframe, code, iframe_addr, code_addr)) // GCOV_EXCL_START
        || fail(copy_field_v(self->ref, iframe, prev_instr, iframe_addr, instr_ptr)))
        return;
    // GCOV_EXCL_STOP

    // f_executable is a _PyStackRef tagged pointer, not a raw PyCodeObject*,
    // on any build new enough to have it tagged at all.
    if (self->free_threaded)
        code_addr = (raddr_t)((uintptr_t)code_addr & ~(uintptr_t)3);
    else if (V_MIN(3, 15))
        code_addr = (raddr_t)((uintptr_t)code_addr & ~(uintptr_t)1);

    // prev_instr is a raw pointer into the code object's bytecode array, not
    // a ready lasti -- convert via V_LASTI like every other unwind path (see
    // _py_thread__push_local_iframe). Skipping this doesn't just mis-resolve
    // the line: it also feeds entry->top's change-detection in
    // _py_asyncio__emit_task a nonsense value.
    uintptr_t lasti = (uintptr_t)V_LASTI(instr_ptr, code_addr);

    // FNV-1a mix of this hop's (code, lasti) into the running fingerprint
    // (see this function's doc comment above).
    *io_chain_fp = (*io_chain_fp ^ (uint64_t)(uintptr_t)code_addr) * 1099511628211ull;
    *io_chain_fp = (*io_chain_fp ^ (uint64_t)lasti) * 1099511628211ull;

    *out_leaf = (task_frame_id_t){iframe_addr, code_addr, lasti, *io_chain_fp};

    // Recurse before appending this frame -- leaf-first push order, per the
    // doc comment above.
    if (_py_asyncio__frame_state_yield_from(py_v->minor, frame_state))
        _py_asyncio__recurse_into_awaited(self, iframe_addr, gen_type_addr, depth, out_leaf, io_chain_fp);

    if (!task_stack_full())
        task_stack_py_push(iframe_addr, code_addr, (int)lasti);
} // _py_asyncio__unwind_coro_chain

// ----------------------------------------------------------------------------
// Read a TaskObj's task_coro and task_name, the two pointers its identity is
// checked against every sample. They are adjacent in TaskObj, so one read
// normally covers both.
static bool
_py_asyncio__read_task_identity(py_proc_t* self, raddr_t task_addr, raddr_t* coro_addr, raddr_t* name_addr) {
    uint64_t o_coro = self->asyncio_offsets.asyncio_task_object.task_coro;
    uint64_t o_name = self->asyncio_offsets.asyncio_task_object.task_name;

    if (o_name == o_coro + sizeof(raddr_t)) {
        raddr_t pair[2];
        if (fail(copy_memory(self->ref, (char*)task_addr + o_coro, sizeof(pair), pair)))
            return false;
        *coro_addr = pair[0];
        *name_addr = pair[1];
        return true;
    }

    return success(copy_asyncio_field(self, task_object, task_coro, task_addr, *coro_addr))  // GCOV_EXCL_LINE
        && success(copy_asyncio_field(self, task_object, task_name, task_addr, *name_addr)); // GCOV_EXCL_LINE
} // _py_asyncio__read_task_identity

// ----------------------------------------------------------------------------
static void
_py_asyncio__emit_task(py_proc_t* self, py_thread_t* thread, raddr_t task_addr) {
    if (!_is_plausible_ptr(task_addr)) // GCOV_EXCL_LINE
        return;                        // GCOV_EXCL_LINE

    task_tracker_entry_t* entry = task_tracker__get_or_create(self->task_tracker, (uintptr_t)task_addr);
    // Tracker full (MAX_TASK_TRACKER concurrently live tasks); best-effort,
    // retry once older tasks are evicted.
    if (!isvalid(entry)) // GCOV_EXCL_LINE
        return;          // GCOV_EXCL_LINE
    entry->last_gen = self->task_tracker->sample_gen;

    // ---- detect task_addr reused by a *different* logical Task ----
    // Eviction runs at end-of-tick, so a Task freed and replaced at the
    // same address within the same tick makes get_or_create above hand
    // back the OLD entry.
    raddr_t coro_addr = NULL;
    raddr_t name_addr = NULL;
    bool    coro_read_ok
        = _py_asyncio__read_task_identity(self, task_addr, &coro_addr, &name_addr) && _is_plausible_ptr(coro_addr);
    // This task's very first sighting (nothing cached yet) -- see the
    // waiter-edge read below, which retries only in this case.
    bool is_first_sighting = coro_read_ok && entry->identity_coro == 0;
    if (coro_read_ok) {
        bool coro_changed = entry->identity_coro != 0 && entry->identity_coro != (uintptr_t)coro_addr;
        // A live task's name changes in two ways: an unnamed task's lazy
        // name (a PyLong counter, never resolved as a str) becoming
        // "Task-<n>" on first get_name(), which is still the same task; and
        // set_name(), which is rare. So a change away from a name already
        // resolved as a str is taken to mean a different task, at the cost
        // of set_name() starting a new identity. Two unnamed tasks reusing
        // the same Task/coroutine pair within one sample still go unnoticed.
        bool name_changed = entry->identity_name != (uintptr_t)name_addr && entry->name_key != 0
                         && entry->name_addr == entry->identity_name;

        // The old task is gone just as surely as if it had been evicted:
        // close it out under its own identity, then start the new one under
        // a fresh one rather than letting it inherit the old wire id.
        if (coro_changed || name_changed) {
            _py_asyncio__flush_task(entry);
            task_tracker__renew(self->task_tracker, entry);
            is_first_sighting = true;
        }
        entry->identity_coro = (uintptr_t)coro_addr;
        entry->identity_name = (uintptr_t)name_addr;
    }

    // Wire identity for this task -- see the id doc comment in
    // task_tracker.h. No remote read needed.
    uintptr_t task_id = _py_asyncio__entry_id(entry);

    // ---- coroutine stack ----
    if (coro_read_ok) {
        V_DESC(self->py_v);

        // The thread's current task, as asyncio itself tracks it -- the one
        // whose code it is running. Not the coroutine frame state: an eager
        // task starts running inside its creator's create_task call, so both
        // coroutines are executing at once, while the current task is the
        // inner one only.
        bool is_running = isvalid(thread) && task_addr == thread->asyncio_task;

        if (is_running) {
            // `thread` is actively running this task's own coroutine right
            // now -- its (possibly deeper, synchronous) call chain is part
            // of `thread`'s live frame chain, not reachable via the
            // async/await-hop walk used for the suspended case below.
            //
            // On a repeat sample, `thread`'s own stack holds nothing but the
            // PYSTACK_REPEAT_MAGIC sentinel -- the split can never succeed, so
            // skip it rather than pay for a doomed scan. `thread` is the very
            // thread running this task's coroutine right now, so its
            // top-of-stack repeat signal doubles as this task's own repeat
            // signal: the task is still at its last reported position. A
            // failed split (e.g. a torn remote read) leaves it there too. The
            // report emitted here carries the time the task was suspended
            // before it started running, if any.
            if (thread->is_repeat) {
                // A running task first seen on a repeat sample (e.g. asyncio
                // discovered halfway through a long blocking call) can't be
                // reported, and so can't be referred to as running either.
                // Have the thread's next sample unwound in full, so that it
                // can be split off and reported then, rather than when its
                // top frame next changes, however long that takes.
                if (!entry->reported) {
                    thread_tracker_entry_t* tentry = thread_tracker__get_or_create(self->thread_tracker, thread->tid);
                    if (isvalid(tentry))
                        tentry->top.frame = NULL;
                }
            } else {
                raddr_t task_iframe = (raddr_t)((char*)coro_addr + py_v->py_gen.o_gi_iframe);

                task_stack_reset();
                if (py_thread__split_task_stack_at(thread, task_iframe)) {
                    _py_asyncio__resolve_task_name(self, task_addr, entry);
                    // First-ever report has no prior position to credit --
                    // same rule as the suspended branch below.
                    event_handler__emit_task_stack_begin(task_id, entry->name_key);
                    event_handler__emit_task_stack_end();
                    entry->reported = true;
                }
            }

            // Forces the next SUSPENDED sighting to count as a change rather
            // than be compared against a now-stale position, even if it is
            // back at the very same chain. It is NOT a first sighting though:
            // that is entry->reported's job.
            entry->top = (task_frame_id_t){0};
        } else {
            py_thread_t     coro_thread = py_thread__init(self);
            task_frame_id_t leaf        = {0};
            uint64_t        chain_fp    = 14695981039346656037ull; // FNV-1a offset basis

            task_stack_reset();
            _py_asyncio__unwind_coro_chain(self, coro_addr, 0, &leaf, &chain_fp);

            bool changed = !isvalid(entry->top.frame) || entry->top.chain_fp != leaf.chain_fp;

            if (changed && isvalid(leaf.frame)) {
                if (success(py_thread__resolve_task_stack(&coro_thread))) {
                    _py_asyncio__resolve_task_name(self, task_addr, entry);
                    event_handler__emit_task_stack_begin(task_id, entry->name_key);
                    event_handler__emit_task_stack_end();
                    entry->reported = true;
                }

                entry->top = leaf;
            }
        }
    } else { // GCOV_EXCL_START
        entry->top = (task_frame_id_t){0};
    } // GCOV_EXCL_STOP

    // ---- waiter edges: only re-emit the (possibly multi-edge) waiter set
    // when its cheap fingerprint has changed since last seen. ----
    raddr_t awaited_by     = NULL;
    char    awaited_by_set = 0;
    bool    waiter_read_ok = false;
    // On first sighting, a torn read here leaves this task's identity on
    // the wire with no waiter edge, which a tree-building consumer reads
    // as a spurious root until a later tick's read succeeds. Retry a few
    // times now instead -- cheap, since every later tick (the common
    // case) still takes one attempt, same as before.
    for (int attempt = 0; attempt < (is_first_sighting ? 4 : 1) && !waiter_read_ok; attempt++) {
        waiter_read_ok
            = success(copy_asyncio_field(self, task_object, task_awaited_by, task_addr, awaited_by))
           && success(copy_asyncio_field(self, task_object, task_awaited_by_is_set, task_addr, awaited_by_set));
    }
    if (waiter_read_ok) {
        uintptr_t fp = _py_asyncio__waiter_fingerprint(self, awaited_by, awaited_by_set);

        if (fp != entry->waiter_fp) {
            entry->waiter_fp = fp;

            if (isvalid(awaited_by)) {
                if (awaited_by_set)
                    _py_asyncio__emit_waiter_set(self, task_addr, task_id, awaited_by);
                else
                    event_handler__emit_task_waiter(task_id, _py_asyncio__task_identity(self, awaited_by));
            } else {
                // The task has lost its last waiter: 0 stands for no task. The
                // waiter may well still be alive (e.g. asyncio.wait() timing
                // out), so readers can't infer this from it going away.
                event_handler__emit_task_waiter(task_id, 0);
            }
        }
    }
} // _py_asyncio__emit_task

// ----------------------------------------------------------------------------
void
py_asyncio__scan_tasks_begin(py_proc_t* self) {
    self->task_tracker->sample_gen++;
    self->task_tracker->scan_complete = true;
} // py_asyncio__scan_tasks_begin

// ----------------------------------------------------------------------------
typedef void (*task_visitor_t)(py_proc_t*, py_thread_t*, raddr_t);

// The tasks a walk didn't reach weren't seen, but they aren't gone either:
// keep py_asyncio__scan_tasks_end from evicting them this sweep.
static inline void
_py_asyncio__walk_cut_short(py_proc_t* self) {
    self->task_tracker->scan_complete = false;
}

static void
_py_asyncio__walk_task_list(py_proc_t* self, py_thread_t* thread, raddr_t list_head_addr, task_visitor_t visit) {
    V_DESC(self->py_v);

    raddr_t node = NULL;
    if (fail(copy_field_v(self->ref, llist, next, list_head_addr, node))) { // GCOV_EXCL_START
        log_d("Cannot read asyncio task list head at %p", list_head_addr);
        _py_asyncio__walk_cut_short(self);
        return;
    } // GCOV_EXCL_STOP

    for (int i = 0; i < MAX_TASK_LIST_ITER && isvalid(node) && node != list_head_addr; i++) {
        if (!_is_plausible_ptr(node)) { // GCOV_EXCL_START
            log_d("Garbled asyncio task list node at %p; aborting this list", node);
            _py_asyncio__walk_cut_short(self);
            return;
        } // GCOV_EXCL_STOP

        raddr_t task_addr = (raddr_t)((char*)node - self->asyncio_offsets.asyncio_task_object.task_node);
        visit(self, thread, task_addr);

        raddr_t next_node = NULL;
        if (fail(copy_field_v(self->ref, llist, next, node, next_node))) { // GCOV_EXCL_START
            log_d("Torn read while walking asyncio task list at node %p", node);
            _py_asyncio__walk_cut_short(self);
            return;
        } // GCOV_EXCL_STOP
        node = next_node;
    }

    // Anything but the head means the walk didn't go all the way round: most
    // likely a NULL next, which is what a node unlinked while being read
    // (e.g. a task completing) leaves behind.
    if (node != list_head_addr) {
        log_d("Asyncio task list at %p walked only in part", list_head_addr);
        _py_asyncio__walk_cut_short(self);
    }
} // _py_asyncio__walk_task_list

// ----------------------------------------------------------------------------
void
py_asyncio__scan_task_list(py_proc_t* self, py_thread_t* thread, raddr_t list_head_addr) {
    if (isvalid(thread)) {
        // One read per thread tells which of its tasks is running. If it
        // fails, every task is treated as suspended for this sample.
        raddr_t running_task_addr
            = (raddr_t)((char*)thread->addr + self->asyncio_offsets.asyncio_thread_state.asyncio_running_task);
        thread->asyncio_task = NULL;
        if (fail(copy_datatype(self->ref, running_task_addr, thread->asyncio_task))) // GCOV_EXCL_LINE
            thread->asyncio_task = NULL;                                             // GCOV_EXCL_LINE
    }

    _py_asyncio__walk_task_list(self, thread, list_head_addr, _py_asyncio__emit_task);

    // After the thread's task reports, so that the running task's latest
    // position is on the wire before this refers to it. Only a task that has
    // been reported can be referred to: one first seen on a repeat sample, or
    // whose split failed, has no position yet, so the sample's time stays the
    // thread's alone.
    if (isvalid(thread) && isvalid(thread->asyncio_task)) {
        task_tracker_entry_t* entry = task_tracker__get(self->task_tracker, (uintptr_t)thread->asyncio_task);
        if (isvalid(entry) && entry->reported)
            event_handler__emit_task_running(_py_asyncio__entry_id(entry));
    }
} // py_asyncio__scan_task_list

// ----------------------------------------------------------------------------
static void
_py_asyncio__keep_task_alive(py_proc_t* self, py_thread_t* thread, raddr_t task_addr) {
    (void)thread;
    task_tracker__keep_alive(self->task_tracker, (uintptr_t)task_addr);
} // _py_asyncio__keep_task_alive

// ----------------------------------------------------------------------------
void
py_asyncio__keep_task_list_alive(py_proc_t* self, raddr_t list_head_addr) {
    _py_asyncio__walk_task_list(self, NULL, list_head_addr, _py_asyncio__keep_task_alive);
} // py_asyncio__keep_task_list_alive

// ----------------------------------------------------------------------------
void
py_asyncio__scan_tasks_end(py_proc_t* self) {
    task_tracker_t* tracker = self->task_tracker;

    // Tasks a walk didn't reach would look gone. Leave eviction to the next
    // sweep that sees every list in full; a task that really is gone will
    // still be missing then.
    if (!tracker->scan_complete)
        return;

    task_tracker_entry_t* stale[MAX_TASK_TRACKER];
    size_t                n = task_tracker__collect_stale(tracker, stale, MAX_TASK_TRACKER);

    for (size_t i = 0; i < n; i++) {
        task_tracker_entry_t* entry = stale[i];

        // Task is gone from the list -- flush before discarding.
        _py_asyncio__flush_task(entry);
        task_tracker__remove(tracker, entry->task);
    }
} // py_asyncio__scan_tasks_end
