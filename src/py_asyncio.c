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
    if (fail(copy_remote(self->ref, self->map.asyncio_debug.base, self->asyncio_offsets))) {
        log_d("Cannot read AsyncioDebug offsets from remote process");
        return;
    }

    if (!_py_asyncio__validate(&self->asyncio_offsets)) {
        log_d("AsyncioDebug offsets failed sanity check; discarding");
        self->map.asyncio_debug.base = NULL; // Retry from scratch next time.
        return;
    }

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
static void
_py_asyncio__emit_waiter_set(py_proc_t* self, raddr_t task_addr, raddr_t set_addr) {
    py_set_t set = {0};
    if (!py_set__read(self->ref, set_addr, self->py_v, &set)) {
        log_d("Cannot read waiter set for task at %p", task_addr);
        return;
    }

    if (!py_set__is_valid(&set)) {
        log_d("Invalid waiter set for task at %p (corrupted remote memory); skipping", task_addr);
        return;
    }

    py_set_iter_t it = {0};
    raddr_t       key;
    while (py_set__next(self->ref, &set, &it, &key))
        event_handler__emit_task_waiter((uintptr_t)task_addr, (uintptr_t)key);

    if (it.seen < set.used)
        log_d("Torn read while walking waiter set for task at %p", task_addr);
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
    if (!py_set__read(self->ref, awaited_by, self->py_v, &set))
        return 0;

    return ((uintptr_t)set.table << 2) ^ ((uintptr_t)set.mask << 1) ^ (uintptr_t)set.used;
} // _py_asyncio__waiter_fingerprint

// ----------------------------------------------------------------------------
// Resolve and cache a task's name (TaskObj.task_name, a plain str -- no
// threading._active-style indirection needed). Reuses string_cache and
// emit_new_string like code.h's filename/scope caching.
static key_dt
_py_asyncio__task_name_key(py_proc_t* self, raddr_t task_addr) {
    V_DESC(self->py_v);

    raddr_t name_addr = NULL;
    if (fail(copy_asyncio_field(self, task_object, task_name, task_addr, name_addr)) || !_is_plausible_ptr(name_addr))
        return 0;

    key_dt           string_key = ptr_key(name_addr);
    cached_string_t* cached     = (cached_string_t*)lru_cache__maybe_hit(self->string_cache, string_key);
    if (isvalid(cached))
        return string_key;

    char* name_value = _string_remote(self->ref, name_addr, py_v);
    if (!isvalid(name_value))
        return 0;

    cached_string_t* new_string = cached_string_new(string_key, name_value);
    if (!isvalid(new_string)) { // GCOV_EXCL_START
        free(name_value);
        return 0;
    } // GCOV_EXCL_STOP

    lru_cache__store(self->string_cache, string_key, new_string);
    event_handler__emit_new_string(new_string);

    return string_key;
} // _py_asyncio__task_name_key

// PyFrameState's numbering (pycore_frame.h) isn't stable across minor
// versions:
//   3.14.x:  CREATED=-3, SUSPENDED=-2, SUSPENDED_YIELD_FROM=-1, EXECUTING=0,
//            COMPLETED=1, CLEARED=4 (no distinct "locked" variant)
//   3.15+:   CREATED=0, SUSPENDED=1, SUSPENDED_YIELD_FROM=2,
//            SUSPENDED_YIELD_FROM_LOCKED=3, EXECUTING=4, CLEARED=5
// Hence these two helpers keyed off py_v->minor, rather than a hardcoded
// constant that would misclassify states on whichever version it wasn't
// written for.
static inline bool
_py_asyncio__frame_state_done(int minor, int8_t frame_state) {
    // 3.14: FRAME_STATE_FINISHED(S) is (S) >= FRAME_COMPLETED(1).
    // 3.15+: only FRAME_CLEARED(5) means "don't touch this frame again".
    return minor <= 14 ? frame_state >= 1 : frame_state == 5;
}

static inline bool
_py_asyncio__frame_state_yield_from(int minor, int8_t frame_state) {
    return minor <= 14 ? frame_state == -1 : (frame_state == 2 || frame_state == 3);
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
    if (fail(copy_field_v(self->ref, iframe, stackpointer, iframe_addr, stackpointer)) || stackpointer == 0)
        return;

    // The awaited object sits one stack slot below the top on 3.14, but two
    // slots below on 3.15.
    uintptr_t slots_back = V_MAX(3, 14) ? 1 : 2;

    uintptr_t awaited_raw = 0;
    if (fail(copy_remote(self->ref, (raddr_t)(stackpointer - slots_back * sizeof(void*)), awaited_raw)))
        return;

    raddr_t awaited_addr = UNTAG_STACKREF(awaited_raw);
    if (!_is_plausible_ptr(awaited_addr))
        return;

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
    if (depth >= MAX_CORO_CHAIN_DEPTH || !_is_plausible_ptr(coro_addr))
        return;

    V_DESC(self->py_v);

    int8_t frame_state = 0;
    if (fail(copy_field_v(self->ref, gen, gi_frame_state, coro_addr, frame_state))
        || _py_asyncio__frame_state_done(py_v->minor, frame_state))
        return;

    raddr_t gen_type_addr = NULL;
    if (fail(copy_remote(self->ref, (char*)coro_addr + py_v->py_object_o_type, gen_type_addr)))
        return;

    raddr_t iframe_addr = (raddr_t)((char*)coro_addr + py_v->py_gen.o_gi_iframe);

    raddr_t code_addr = NULL;
    raddr_t instr_ptr = NULL;
    if (fail(copy_field_v(self->ref, iframe, code, iframe_addr, code_addr))
        || fail(copy_field_v(self->ref, iframe, prev_instr, iframe_addr, instr_ptr)))
        return;

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
static void
_py_asyncio__emit_task(py_proc_t* self, raddr_t task_addr, microseconds_t time_delta) {
    if (!_is_plausible_ptr(task_addr))
        return;

    task_tracker_entry_t* entry = task_tracker__get_or_create(self->task_tracker, (uintptr_t)task_addr);
    if (!isvalid(entry)) // Tracker full; best-effort, retry once older tasks are evicted.
        return;
    entry->last_gen = self->task_tracker->sample_gen;

    // Accrue elapsed time onto the task's current suspension point; flushed
    // once that point changes (attributed to the old frame), or discarded on
    // a torn/failed coroutine read (no frame left to attribute it to).
    entry->suspended_time += time_delta;

    // ---- coroutine stack: walk the whole chain every scan (cheap raw
    // pointer reads, no name resolution) for the leaf's identity and a
    // fingerprint of the chain above it -- the leaf alone can look unchanged
    // while the task still progresses (see task_frame_id_t in
    // task_tracker.h). Only resolve/emit when the fingerprint changes. ----
    raddr_t coro_addr = NULL;
    if (success(copy_asyncio_field(self, task_object, task_coro, task_addr, coro_addr))
        && _is_plausible_ptr(coro_addr)) {
        py_thread_t     coro_thread = py_thread__init(self);
        task_frame_id_t leaf        = {0};
        uint64_t        chain_fp    = 14695981039346656037ull; // FNV-1a offset basis

        task_stack_reset();
        _py_asyncio__unwind_coro_chain(self, coro_addr, 0, &leaf, &chain_fp);

        bool has_prior_frame = isvalid(entry->top.frame);
        bool changed         = !has_prior_frame || entry->top.chain_fp != leaf.chain_fp;

        if (changed && isvalid(leaf.frame)) {
            // First-ever sighting has no prior frame to attribute time to --
            // discard rather than credit an unobserved frame.
            uint64_t elapsed = has_prior_frame ? entry->suspended_time : 0;

            if (success(py_thread__resolve_task_stack(&coro_thread))) {
                key_dt name_key = _py_asyncio__task_name_key(self, task_addr);
                // Keep the last known-good name rather than clobbering it
                // with 0, so a transient failure doesn't erase the eviction
                // fallback.
                if (name_key != 0)
                    entry->name_key = (uintptr_t)name_key;
                event_handler__emit_task_stack_begin((uintptr_t)task_addr, (uintptr_t)name_key);
                event_handler__emit_task_stack_end(elapsed);
                entry->suspended_time = 0;
            }

            entry->top = leaf;
        }
    } else {
        entry->top            = (task_frame_id_t){0};
        entry->suspended_time = 0;
    }

    // ---- waiter edges: only re-emit the (possibly multi-edge) waiter set
    // when its cheap fingerprint has changed since last seen. ----
    raddr_t awaited_by     = NULL;
    char    awaited_by_set = 0;
    if (success(copy_asyncio_field(self, task_object, task_awaited_by, task_addr, awaited_by))
        && success(copy_asyncio_field(self, task_object, task_awaited_by_is_set, task_addr, awaited_by_set))) {
        uintptr_t fp = _py_asyncio__waiter_fingerprint(self, awaited_by, awaited_by_set);

        if (fp != entry->waiter_fp) {
            entry->waiter_fp = fp;

            if (isvalid(awaited_by)) {
                if (awaited_by_set)
                    _py_asyncio__emit_waiter_set(self, task_addr, awaited_by);
                else
                    event_handler__emit_task_waiter((uintptr_t)task_addr, (uintptr_t)awaited_by);
            }
            // fp == 0 (no waiters): nothing to emit; consumers infer the loss
            // of a waiter edge from the waiter task's own disappearance.
        }
    }
} // _py_asyncio__emit_task

// ----------------------------------------------------------------------------
void
py_asyncio__scan_tasks_begin(py_proc_t* self) {
    self->task_tracker->sample_gen++;
} // py_asyncio__scan_tasks_begin

// ----------------------------------------------------------------------------
void
py_asyncio__scan_task_list(py_proc_t* self, raddr_t list_head_addr, microseconds_t time_delta) {
    V_DESC(self->py_v);

    raddr_t node = NULL;
    if (fail(copy_field_v(self->ref, llist, next, list_head_addr, node))) {
        log_d("Cannot read asyncio task list head at %p", list_head_addr);
        return;
    }

    for (int i = 0; i < MAX_TASK_LIST_ITER && isvalid(node) && node != list_head_addr; i++) {
        if (!_is_plausible_ptr(node)) {
            log_d("Garbled asyncio task list node at %p; aborting this list", node);
            return;
        }

        raddr_t task_addr = (raddr_t)((char*)node - self->asyncio_offsets.asyncio_task_object.task_node);
        _py_asyncio__emit_task(self, task_addr, time_delta);

        raddr_t next_node = NULL;
        if (fail(copy_field_v(self->ref, llist, next, node, next_node))) {
            log_d("Torn read while walking asyncio task list at node %p", node);
            return;
        }
        node = next_node;
    }
} // py_asyncio__scan_task_list

// ----------------------------------------------------------------------------
void
py_asyncio__scan_tasks_end(py_proc_t* self) {
    task_tracker_t* tracker = self->task_tracker;

    task_tracker_entry_t* stale[MAX_TASK_TRACKER];
    size_t                n = task_tracker__collect_stale(tracker, stale, MAX_TASK_TRACKER);

    for (size_t i = 0; i < n; i++) {
        task_tracker_entry_t* entry = stale[i];

        // Task is gone from the list -- flush its accrued dwell time
        // before discarding rather than losing it silently. No remote
        // reads needed (frame/name already cached), safe even if the
        // TaskObj has since been freed. Empty frame sequence signals a
        // closing metric only, no new stack content.
        if (isvalid(entry->top.frame) && entry->suspended_time > 0) {
            task_stack_reset();
            event_handler__emit_task_stack_begin((uintptr_t)entry->task, entry->name_key);
            event_handler__emit_task_stack_end(entry->suspended_time);
        }
        task_tracker__remove(tracker, entry->task);
    }
} // py_asyncio__scan_tasks_end
