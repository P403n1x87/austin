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

// Phase 1 of asyncio-aware profiling: locate and validate the AsyncioDebug
// debug offsets exposed by the _asyncio extension module (CPython 3.14+), so
// that later phases can read a task's name and its await-chain from raw
// process memory.

#pragma once

#include "py_proc.h"

// The _asyncio module is loaded lazily (on `import asyncio`), so the
// platform's debug-section scan is retried on a Fibonacci backoff: fast at
// first, to catch a typical early `import asyncio` promptly, then capped at
// ASYNCIO_SCAN_BACKOFF_CAP_US so it doesn't grow unbounded. After
// ASYNCIO_SCAN_BACKOFF_WINDOW_US of no success, it gives up on fast retries
// and falls back to a sparse, near-zero-cost poll (ASYNCIO_SCAN_SPARSE_
// INTERVAL_US) for the rest of the process's life -- so a late or
// conditional `import asyncio` still eventually gets picked up, just not
// promptly, while a process that never imports it at all costs almost
// nothing.
#define ASYNCIO_SCAN_BACKOFF_MIN_US     1000    // 1 ms -- first retry interval
#define ASYNCIO_SCAN_BACKOFF_CAP_US     100000  // 100 ms -- backoff never grows past this
#define ASYNCIO_SCAN_BACKOFF_WINDOW_US  1000000 // 1 s -- how long to use the fast backoff before falling back
#define ASYNCIO_SCAN_SPARSE_INTERVAL_US 5000000 // 5 s -- fallback cadence once the fast window has elapsed

/**
 * Read and sanity-check a just-located AsyncioDebug section's contents.
 *
 * On success, caches the offsets and marks asyncio support as available.
 * Best-effort: a failed read or a failed sanity check just resets the
 * section address so the scan is retried from scratch later, rather than
 * treating a torn read (e.g. mid-import) as permanent.
 *
 * @param self the process object.
 */
void
py_asyncio__validate_and_cache(py_proc_t* self);

// Phase 2: capture the full waiter DAG for every live (suspended or running)
// task known to the interpreter, plus the coroutine stack of every suspended
// task whose top-frame identity has changed since the last scan.
//
// This walks the *whole* task population and is comparatively expensive, so
// callers call it once per interpreter per sample, not once per thread. The
// three functions below are always called together, in this order,
// bracketing every task list belonging to a given interpreter for the
// current sample:
//   1. py_asyncio__scan_tasks_begin, once, before any list is walked.
//   2. py_asyncio__scan_task_list, once per list (see the two head-address
//      helpers below for the lists a sample may need to walk).
//   3. py_asyncio__scan_tasks_end, once, after every list has been walked.
//
// Best-effort throughout: bounded iteration counts and pointer sanity checks
// guard against torn reads and reference cycles in the remote task graph;
// a bad list or a bad task is skipped rather than aborting the whole scan.

/**
 * Begin a Phase-2 task-population scan for one interpreter.
 *
 * Marks the current sample generation so that, once every list has been
 * walked, tasks not seen this time around can be told apart from ones
 * that are still alive.
 *
 * @param self the process object.
 */
void
py_asyncio__scan_tasks_begin(py_proc_t* self);

/**
 * Walk one asyncio task list, emitting/updating every task found on it.
 *
 * The list is either a thread's own task list, or the interpreter-wide
 * fallback list that a thread's still-pending tasks get moved to when that
 * thread is destroyed (see the two head-address helpers below). For every
 * task found, resolves its identity and, if its frame or waiter set has
 * changed, unwinds and emits it. Bails out on the first implausible
 * pointer, torn read, or excessive iteration count, so a corrupted or
 * cyclic remote list can't hang or crash the sampler.
 *
 * @param self           the process object.
 * @param list_head_addr the remote address of the list's llist_node
 *                       sentinel (see py_asyncio__interp_task_list_head/
 *                       py_asyncio__thread_task_list_head below).
 * @param time_delta     elapsed time since the previous sample, in the
 *                       same unit and mode (wall/CPU) as the rest of
 *                       Austin's sampling loop; credited to a task's dwell
 *                       time at its current frame.
 */
void
py_asyncio__scan_task_list(py_proc_t* self, raddr_t list_head_addr, microseconds_t time_delta);

/**
 * End a Phase-2 task-population scan, evicting tasks that disappeared.
 *
 * Evicts every tracked task not seen in this sample's scan (completed, or
 * otherwise removed from the interpreter's task population). Before
 * dropping one, flushes whatever dwell time it had accrued at its last
 * known frame as a closing metric with an empty frame sequence, so that
 * time isn't silently lost; a task with nothing accrued is just dropped.
 *
 * @param self the process object.
 */
void
py_asyncio__scan_tasks_end(py_proc_t* self);

/**
 * Address of an interpreter's asyncio task list head.
 *
 * The head to pass to py_asyncio__scan_task_list for the interpreter-wide
 * fallback list -- normally empty; only holds tasks whose owning thread has
 * been destroyed since (see py_asyncio__scan_task_list's own doc comment).
 * Trivial pointer arithmetic, inlined so callers in the hot sampling loop
 * don't pay a function-call for it.
 *
 * @param self        the process object.
 * @param interp_addr the remote address of the interpreter state.
 *
 * @return the remote address of the list's llist_node sentinel.
 */
static inline raddr_t
py_asyncio__interp_task_list_head(py_proc_t* self, raddr_t interp_addr) {
    return (raddr_t)((char*)interp_addr + self->asyncio_offsets.asyncio_interpreter_state.asyncio_tasks_head);
}

/**
 * Address of a thread's own asyncio task list head.
 *
 * Same as py_asyncio__interp_task_list_head, but for the list a thread
 * actually keeps its own tasks on by default (on every build, not just
 * free-threaded ones) to avoid cross-thread contention.
 *
 * @param self        the process object.
 * @param tstate_addr the remote address of the thread's PyThreadState.
 *
 * @return the remote address of the list's llist_node sentinel.
 */
static inline raddr_t
py_asyncio__thread_task_list_head(py_proc_t* self, raddr_t tstate_addr) {
    return (raddr_t)((char*)tstate_addr + self->asyncio_offsets.asyncio_thread_state.asyncio_tasks_head);
}
