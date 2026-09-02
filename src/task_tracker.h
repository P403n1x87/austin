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

#define MAX_TASK_TRACKER 1024

typedef struct {
    void*     frame; // remote address of the coroutine's leaf iframe (NULL = no prior emit)
    void*     code;  // code object at frame
    uintptr_t lasti;
    uint64_t  chain_fp; // fingerprint of the whole await chain from root to this leaf; see comment above
} task_frame_id_t;

typedef struct {
    uintptr_t       task;      // remote address of the TaskObj (key)
    task_frame_id_t top;       // pre-unwind coroutine frame identity
    uintptr_t       waiter_fp; // cheap fingerprint of task_awaited_by; 0 = no waiters
    unsigned int    last_gen;
    // Wall/CPU time (same unit as sample_t.time) accumulated across scans
    // while `top` stayed unchanged, i.e. how long the task has been sitting
    // at its current suspension point. Flushed and reset to 0 as soon as
    // `top` is next observed to change (see _py_asyncio__emit_task) -- it
    // then describes how long the task dwelled at the frame it just left.
    uint64_t        suspended_time;
    // Cached MOJO_TASK_STACK name key from the last successful resolution (0
    // = never resolved), so that flushing suspended_time on eviction (the
    // task has disappeared from the list, e.g. it completed) never needs to
    // re-read the TaskObj's name -- by then it may already be freed.
    uintptr_t       name_key;
} task_tracker_entry_t;

typedef struct {
    task_tracker_entry_t entries[MAX_TASK_TRACKER];
    size_t               count;
    unsigned int         sample_gen;
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
 * Returns NULL only if the tracker is full (> MAX_TASK_TRACKER tasks).
 */
task_tracker_entry_t*
task_tracker__get_or_create(task_tracker_t*, uintptr_t task);

/**
 * Remove the entry at index i via swap-with-last, keeping the array compact.
 * Callers scanning for stale entries (last_gen < sample_gen) should flush any
 * pending state on an entry (e.g. suspended_time) *before* removing it -- see
 * py_asyncio__scan_tasks_end, which owns that policy since it needs the
 * py_proc_t context to emit anything.
 */
void
task_tracker__remove_at(task_tracker_t*, size_t i);

/**
 * Destroy the task tracker and free all associated memory.
 */
void
task_tracker__destroy(task_tracker_t*);
