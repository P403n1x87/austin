// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

// Per-task identity tracker: detects whether a suspended asyncio task's
// coroutine stack or waiter set has changed since the last scan, so
// unchanged tasks can be skipped instead of paying for a full unwind and
// waiter-set read every sample. Dead tasks are evicted by comparing
// last_gen against sample_gen, which is bumped at the start of each sweep.
//
// Frame identity (top) needs more than just the leaf frame's own address:
// a shared primitive like asyncio.sleep() always suspends at the same
// bytecode position, and CPython's allocator routinely reuses the same
// freed slot for the next allocation, so two unrelated sleep() calls can
// share both a frame address and a leaf position. chain_fp folds in every
// hop's (code, lasti) from the task's own coroutine down to the leaf, so
// the shape of the call path above it is part of the identity too --
// different callers of the same shared primitive compare as different.
//
// waiter_fp is a cheap fingerprint of task_awaited_by (the single waiter
// pointer, or a fold of the waiter set's table/mask/count), enough to
// detect additions/removals/replacements without reading every element.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cache.h" // lookup_t

// Safety valve, not a realistic expectation: bounds memory/lookup-table
// growth against a corrupted or adversarially-mutating remote task graph,
// the same role MAX_TASK_TRACKER played when entries lived in a flat array.
#define MAX_TASK_TRACKER 1024

typedef struct {
    void*     frame; // remote address of the coroutine's leaf iframe (NULL = nothing to compare against)
    void*     code;  // code object at frame
    uintptr_t lasti;
    uint64_t  chain_fp; // fingerprint of the whole await chain from root to this leaf; see comment above
} task_frame_id_t;

typedef struct {
    uintptr_t       task;      // remote address of the TaskObj (key)
    task_frame_id_t top;       // pre-unwind coroutine frame identity
    uintptr_t       waiter_fp; // cheap fingerprint of task_awaited_by; 0 = no waiters
    unsigned int    last_gen;
    // Time (same unit as sample_t.time) accrued since this task's last
    // report, i.e. at the position that report showed. The next report
    // carries it and resets it (see _py_asyncio__emit_task), whether the
    // task is then running or suspended. Running samples that can't be
    // reported (a repeat sample, which has nothing to split against, or a
    // failed split) keep adding to it, so a long synchronous stretch inside
    // a coroutine is still credited in full once it ends. In CPU mode it
    // only grows while the task is running.
    uint64_t        pending_time;
    // Cached MOJO_TASK_STACK name key from the last successful resolution (0
    // = never resolved), so that flushing pending_time on eviction (the
    // task has disappeared from the list, e.g. it completed) never needs to
    // re-read the TaskObj's name -- by then it may already be freed.
    uintptr_t       name_key;
    // Remote address of the str name_key was resolved from, so that the
    // name of a live task (which keeps its str alive) isn't re-read every
    // time it's reported. See _py_asyncio__resolve_task_name.
    uintptr_t       name_addr;
    // TaskObj's task_coro as of the last touch (0 = never touched). `task`
    // (the lookup key) only identifies reliably as long as its memory isn't
    // reused -- eviction runs at end-of-tick, so a freed Task replaced at
    // the same address within the same tick makes get_or_create hand back
    // THIS entry. A coro mismatch signals every other cached field
    // (waiter_fp above all) is stale and must be reset before use.
    uintptr_t       identity_coro;
    // TaskObj's task_name as of the last touch. The coro alone misses a
    // replacement whose coroutine also landed at the old one's address --
    // pymalloc often hands out a Task/coroutine pair together. A change of
    // name away from one already resolved as a str catches most of those
    // (see _py_asyncio__emit_task).
    uintptr_t       identity_name;
    // The task's wire identity: a sequential number, assigned when this
    // entry is first allocated, and again by task_tracker__renew when a
    // different task turns out to live at the same address. Nothing derived
    // from `task` is safe here: pymalloc reusing a freed TaskObj's address
    // for an unrelated Task is routine, and mojo_ref truncates to 27 bits
    // (mojo.h). Folding a counter into the address (by XOR) still collided,
    // since allocators hand out nearby addresses while the counter takes
    // small values. Drawn from a single counter shared by every tracker, so
    // ids are also unique across the processes of a multiprocess session,
    // until the counter wraps at 2^27 tasks.
    unsigned int    id;
    // Whether this task has been emitted at least once, running or
    // suspended. Decides the first-sighting rule (a task's very first report
    // has no previous position to credit, so it carries 0 elapsed time) and
    // nothing else. `top` can't double as this signal: it is also cleared
    // after every EXECUTING report, purely to force the next SUSPENDED
    // sighting to count as a change, which would otherwise make that
    // sighting look like a first one and drop the running position's time.
    bool            reported;
} task_tracker_entry_t;

typedef struct {
    lookup_t*    by_task; // task address -> heap-allocated task_tracker_entry_t*
    size_t       count;   // live entry count; tracked here so MAX_TASK_TRACKER
                          // can be enforced without reaching into lookup_t
    unsigned int sample_gen;
    // Whether every task list walked this sweep was walked in full. A walk
    // cut short (e.g. by a node being unlinked as it is read) doesn't touch
    // the tasks it didn't reach, which must not be taken for gone.
    bool         scan_complete;
} task_tracker_t;

/**
 * Create a new task tracker.
 *
 * @return a pointer to the newly created task_tracker_t, or NULL on failure.
 */
task_tracker_t*
task_tracker_new(void);

/**
 * Return the existing entry for task, or allocate and return a new one.
 * Returns NULL only if the tracker is full (> MAX_TASK_TRACKER tasks) or on
 * allocation failure.
 */
task_tracker_entry_t*
task_tracker__get_or_create(task_tracker_t*, uintptr_t task);

/**
 * Reset `entry` for a different task found at the same address (see
 * identity_coro), keeping only its key and liveness, and assign it a fresh
 * id so the new task gets its own wire identity instead of inheriting the
 * old one's.
 */
void
task_tracker__renew(task_tracker_t*, task_tracker_entry_t* entry);

/**
 * Mark the existing entry for `task`, if any, as seen in the current sample
 * generation, without creating one, so task_tracker__collect_stale doesn't
 * evict it. For tasks whose owning thread was skipped this sample (e.g. idle
 * in CPU mode): they are still alive, they just weren't scanned.
 */
void
task_tracker__keep_alive(task_tracker_t*, uintptr_t task);

/**
 * Collect up to `max` entries whose last_gen is older than the tracker's
 * current sample_gen (i.e. tasks that disappeared from every list walked
 * this sweep) into `out`. Does NOT remove them: a hash table can't be
 * safely mutated while it's being walked. Pair with task_tracker__remove for
 * each entry collected here, once the caller is done reading it (e.g. to flush
 * its accrued dwell time before it's gone).
 *
 * @return the number of entries collected (never more than `max`).
 */
size_t
task_tracker__collect_stale(task_tracker_t*, task_tracker_entry_t** out, size_t max);

/**
 * Remove and free the entry for `task`. Only safe to call once any
 * in-progress task_tracker__collect_stale walk has returned.
 */
void
task_tracker__remove(task_tracker_t*, uintptr_t task);

/**
 * Destroy the task tracker and free all associated memory.
 */
void
task_tracker__destroy(task_tracker_t*);
