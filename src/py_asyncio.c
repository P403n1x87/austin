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

// AsyncioDebug carries no cookie, so we validate by sanity-checking the
// embedded sizes instead: they must be non-zero and small enough to be a
// plausible object/struct size, never hardcoded (they vary, e.g. on
// free-threaded builds where TaskObj gains a task_tid field).
#define IS_SANE_SIZE(x) ((x) > 0 && (x) <= 4096)

// asyncio_interpreter_state.size is sizeof(PyInterpreterState), not a small
// object size like the other two: PyInterpreterState embeds numerous
// per-interpreter caches (GC state, object free lists, etc.) and is known to
// be on the order of hundreds of KB on modern CPython builds, so it needs a
// much larger bound than IS_SANE_SIZE.
#define IS_SANE_INTERP_SIZE(x) ((x) > 0 && (x) <= (1 << 20))

// Copy a field out of a remote object, at the offset given by one of
// self->asyncio_offsets' three groups (task_object, interpreter_state,
// thread_state -- passed short, the asyncio_ prefix they all share is added
// here) -- the AsyncioDebug equivalent of copy_field_v (mem.h), which
// instead indexes the version descriptor (py_v). raddr is the remote
// address of the object itself, not of the field.
#define copy_asyncio_field(self, group, field, raddr, dst)                                                 \
    copy_memory((self)->ref, (raddr) + (self)->asyncio_offsets.asyncio_##group.field, sizeof(dst), &(dst))

static bool
_py_asyncio__validate(Py_AsyncioModuleDebugOffsets* offsets) {
    return IS_SANE_SIZE(offsets->asyncio_task_object.size)
        && IS_SANE_INTERP_SIZE(offsets->asyncio_interpreter_state.size)
        && IS_SANE_SIZE(offsets->asyncio_thread_state.size);
} // _py_asyncio__validate

// ----------------------------------------------------------------------------
void
py_asyncio__validate_and_cache(py_proc_t* self) {
    // Read straight into the destination: every consumer of
    // self->asyncio_offsets must gate on self->asyncio_debug_found first, so a
    // failed read/validation leaving partial data here is harmless -- it just
    // won't ever be looked at.
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
// Phase 2: task-graph walk (full waiter DAG + suspended-task coroutine
// stacks). See py_asyncio.h for the call-site contract.
//
// Bounds below guard against corrupted/garbled remote memory and reference
// cycles from a task graph that may be concurrently mutated by the target
// process: they cap iteration counts rather than trusting the remote data to
// be well-formed, mirroring (and in the cycle-detection case, improving on)
// the safety valve CPython's own Modules/_remote_debugging/asyncio.c uses.

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
// Cheap fingerprint of task_awaited_by, cheap enough to compute every sample
// without walking a multi-waiter set: identifies the waiter object itself in
// the single-waiter case, or folds the set's table/mask/used triple in the
// multi-waiter case. Either way, any actual add/remove/replace changes it.
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
// Resolve and cache a task's name (TaskObj.task_name, a plain str object —
// unlike thread names there is no threading._active dict indirection to walk).
// Reuses self->string_cache and the emit_new_string hook, exactly like
// code.h's filename/scope caching, so repeat names cost one LRU lookup rather
// than a fresh remote string read.
static key_dt
_py_asyncio__task_name_key(py_proc_t* self, raddr_t task_addr) {
    V_DESC(self->py_v);

    raddr_t name_addr = NULL;
    if (fail(copy_asyncio_field(self, task_object, task_name, task_addr, name_addr)) || !_is_plausible_ptr(name_addr))
        return 0;

    key_dt           string_key = (key_dt)(uintptr_t)name_addr;
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

// PyFrameState's numbering (Include/internal/pycore_frame.h) is NOT stable
// across minor versions -- confirmed directly against each version's own
// tagged source:
//   3.14.x:  CREATED=-3, SUSPENDED=-2, SUSPENDED_YIELD_FROM=-1, EXECUTING=0,
//            COMPLETED=1, CLEARED=4 (no distinct "locked" variant)
//   3.15+:   CREATED=0, SUSPENDED=1, SUSPENDED_YIELD_FROM=2,
//            SUSPENDED_YIELD_FROM_LOCKED=3, EXECUTING=4, CLEARED=5
// Checked via these two helpers (keyed off py_v->minor) instead of hardcoded
// constants for exactly this reason -- a single hardcoded value silently
// misclassifies every state on whichever version it wasn't written for.
static inline bool
_py_asyncio__frame_state_done(int minor, int8_t frame_state) {
    // 3.14: CPython's own FRAME_STATE_FINISHED(S) macro is (S) >= FRAME_COMPLETED(1).
    // 3.15+: only FRAME_CLEARED(5) means "don't touch this frame again".
    return minor <= 14 ? frame_state >= 1 : frame_state == 5;
}

static inline bool
_py_asyncio__frame_state_yield_from(int minor, int8_t frame_state) {
    return minor <= 14 ? frame_state == -1 : (frame_state == 2 || frame_state == 3);
}

// Safety valve against a corrupted or (degenerately) cyclic await chain in
// remote memory -- a legitimate chain is a handful of frames deep at most.
#define MAX_CORO_CHAIN_DEPTH 64

// _PyStackRef tagged-pointer scheme (stable since its introduction in 3.13):
// the low 2 bits carry tag info, the real address is the rest.
#define UNTAG_STACKREF(bits) ((raddr_t)((uintptr_t)(bits) & ~(uintptr_t)0x3))

// ----------------------------------------------------------------------------
// Reconstructs a suspended task's full "await chain" -- the coroutines it is
// transitively awaiting via plain `await expr` (as opposed to a separately
// tracked Task, which has its own entry in the task graph). Unlike a normal
// call stack, this can't be walked via the interpreter frame's `previous`
// pointer: CPython clears that the moment a coroutine suspends (previous
// only reflects an actively-executing C call chain). Instead this mirrors
// CPython's own Modules/_remote_debugging/asyncio.c (parse_coro_chain /
// handle_yield_from_frame), which reads what a suspended coroutine is
// awaiting off its own evaluation stack, where CPython's bytecode keeps a
// live reference to it independent of `previous`:
//
//   1. Read this coroutine's gi_frame_state. Stop if FRAME_CLEARED.
//   2. If gi_frame_state == FRAME_SUSPENDED_YIELD_FROM, peek the frame's own
//      stack one slot below its top (stackpointer[-1]) for what it's
//      awaiting, untag it, and check its type: if it's still the *same*
//      type as this coroutine (i.e. another plain coroutine, not a Task,
//      Future, or other awaitable), recurse into it first. Otherwise stop --
//      that's where the task-level waiter/attachment machinery takes over.
//   3. Push *only* this coroutine's own frame identity (code, lasti) after
//      recursing, so the innermost (leaf) frame gets pushed first and the
//      outermost (root) last -- matching the leaf-first push order every
//      other stack-unwind path in Austin uses (_py_thread__unwind_iframe_stack
//      et al.), which relies on the frame stack's LIFO pop to emit
//      root-first/leaf-last on the wire. Appending root-to-leaf here would
//      invert that ordering.
//
// Deliberately pushes with stack_py_push directly instead of going through
// py_thread__append_iframe_stack (which walks the WHOLE `previous` chain,
// not just one frame): that's exactly right for a normal call-stack unwind,
// but wrong here. A coroutine currently *executing* on some thread (as
// opposed to suspended) still has a live, non-NULL `previous` -- it's
// genuinely part of that thread's real, active C call chain right now. Going
// through py_thread__append_iframe_stack for such a coroutine would walk all
// the way up through the thread's own eval-loop frames (Handle._run,
// BaseEventLoop._run_once, ...), producing a single, garbled, doubled-up
// "task stack" that mixes this coroutine's chain with the thread's own --
// exactly the on-CPU/tall-stack bug this avoids. Each hop here only ever
// wants its OWN single frame; the recursion above is what walks the chain.
//
// Writes the deepest successfully-pushed hop's identity into *out_leaf every
// time, so a best-effort partial walk (a depth-limited or torn read) still
// leaves the caller something usable -- consistent with the rest of this
// file's treatment of remote-memory reads.
//
// *io_chain_fp is a running fingerprint, folding in this hop's (code, lasti)
// on every call regardless of depth or recursion outcome, so it accumulates
// the whole path from root to leaf as the recursion descends. See the
// comment above task_frame_id_t (task_tracker.h) for why the leaf's own
// identity alone can't tell two different callers of the same shared
// coroutine (e.g. two call sites both awaiting asyncio.sleep()) apart: the
// callee's (code, lasti) is intrinsic to the callee, not the caller, and its
// frame address is unreliable since CPython's allocator tends to reuse a
// just-freed same-size slot for the next allocation. Deliberately excludes
// frame addresses entirely -- only (code, lasti) per hop feeds the
// fingerprint, since that's the part that's actually stable per call site.
static void
_py_asyncio__unwind_coro_chain(
    py_proc_t* self, raddr_t coro_addr, int depth, task_frame_id_t* out_leaf, uint64_t* io_chain_fp
);

// Recurse into whatever a coroutine at a SUSPENDED_YIELD_FROM point (see the
// frame_state check at _py_asyncio__unwind_coro_chain's only call site
// below) is awaiting, provided it's still a plain coroutine object (not yet
// a Task/Future/other awaitable) -- comparing the awaited object's type to
// gen_type_addr, the awaiting coroutine's own type, is what makes the walk
// terminate at a Task/Future boundary without a special case for it. Reads
// bail out early (guard-clause style) rather than nesting, since every step
// here depends on the previous one succeeding.
static void
_py_asyncio__recurse_into_awaited(
    py_proc_t* self, raddr_t iframe_addr, raddr_t gen_type_addr, int depth, task_frame_id_t* out_leaf,
    uint64_t* io_chain_fp
) {
    V_DESC(self->py_v);

    uintptr_t stackpointer = 0;
    if (fail(copy_field_v(self->ref, iframe, stackpointer, iframe_addr, stackpointer)) || stackpointer == 0)
        return;

    uintptr_t awaited_raw = 0;
    if (fail(copy_remote(self->ref, (raddr_t)(stackpointer - sizeof(void*)), awaited_raw)))
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

    // prev_instr is a raw pointer into the code object's own bytecode array,
    // not a ready-to-use instruction index -- every other unwind path in
    // Austin converts it via this same conversion before treating it as
    // "lasti" (see V_LASTI in version.h, and _py_thread__push_local_iframe
    // in py_thread.c for its non-TLBC use). Skipping this (an earlier
    // version of this function stored the raw pointer directly) doesn't
    // just give a wrong line number once resolved -- it also feeds a
    // nonsensical value into the entry->top identity comparison in
    // _py_asyncio__emit_task, which uses it to decide whether a task's leaf
    // frame has actually changed since the last scan.
    uintptr_t lasti = (uintptr_t)V_LASTI(instr_ptr, code_addr);

    // FNV-1a-style mix of this hop's (code, lasti) into the running chain
    // fingerprint -- see the comment above this function's declaration.
    *io_chain_fp = (*io_chain_fp ^ (uint64_t)(uintptr_t)code_addr) * 1099511628211ull;
    *io_chain_fp = (*io_chain_fp ^ (uint64_t)lasti) * 1099511628211ull;

    *out_leaf = (task_frame_id_t){iframe_addr, code_addr, lasti, *io_chain_fp};

    // Recurse into whatever this coroutine is awaiting *before* appending its
    // own frame. Austin's other stack-unwind paths (_py_thread__unwind_iframe_stack
    // et al.) always push the leaf frame first and the root last, relying on the
    // frame stack's LIFO pop (mojo_event_handler__handle_task_stack_end) to emit
    // root-first/leaf-last on the wire. Appending here in call order (root first)
    // would invert that -- so instead we resolve the deeper frames first and only
    // append this one on the way back out, matching the leaf-first push order
    // every other stack in Austin uses.
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

    // Accrue this scan's elapsed time onto whichever suspension point the
    // task currently occupies. Flushed as soon as that point is observed to
    // change below (attributed to the frame it was accrued against, not the
    // new one), or discarded on a torn/failed coroutine read, since there is
    // then no frame left to attribute it to.
    entry->suspended_time += time_delta;

    // ---- coroutine stack: walk the whole await chain every scan (cheap --
    // raw pointer reads only, no name resolution yet) to find the *leaf*
    // frame's identity and a fingerprint of the whole chain above it, since
    // the leaf alone can sit still while stale (see the comment above
    // task_frame_id_t in task_tracker.h) even as the task genuinely makes
    // progress. Only pay for the expensive part -- resolving names/scopes
    // and emitting -- when that fingerprint has actually changed since last
    // seen. ----
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
            // No prior frame on the task's first-ever sighting, so there's
            // nothing to attribute the accrued time to; discard it rather
            // than crediting it to a frame we never actually observed.
            uint64_t elapsed = has_prior_frame ? entry->suspended_time : 0;

            if (success(py_thread__resolve_task_stack(&coro_thread))) {
                key_dt name_key = _py_asyncio__task_name_key(self, task_addr);
                // Keep the last known-good name cached (rather than
                // clobbering it with 0) so a transient resolution failure
                // here doesn't erase what we'll fall back on at eviction.
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

    size_t i = 0;
    while (i < tracker->count) {
        task_tracker_entry_t* entry = &tracker->entries[i];

        if (entry->last_gen < tracker->sample_gen) {
            // This task is gone from the list (completed, or otherwise
            // dropped) -- flush its accrued dwell time before discarding it,
            // rather than losing it silently. No remote reads needed: the
            // frame and name were already resolved and cached, so this is
            // safe even if the TaskObj/coroutine has since been freed. The
            // empty frame sequence tells consumers there's no new stack
            // content, just a closing metric for what they already have.
            if (isvalid(entry->top.frame) && entry->suspended_time > 0) {
                task_stack_reset();
                event_handler__emit_task_stack_begin((uintptr_t)entry->task, entry->name_key);
                event_handler__emit_task_stack_end(entry->suspended_time);
            }
            task_tracker__remove_at(tracker, i);
        } else {
            i++;
        }
    }
} // py_asyncio__scan_tasks_end
