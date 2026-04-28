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

// Per-thread stack identity tracker used to detect repeated stacks between
// consecutive samples and emit MOJO_STACK_REPEAT instead of re-emitting the
// full frame sequence.
//
// Python-only mode: identity is the top_frame remote address checked before
// unwinding, so the unwind can be skipped entirely on a repeat.
//
// Native mode: identity is a hash of the resolved stack buffers computed after
// unwinding; only the emit is skipped on a repeat.
//
// Dead threads are evicted by comparing last_gen against sample_gen, which is
// incremented at the start of each full thread sweep.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_THREAD_TRACKER 256

typedef struct {
    uintptr_t    tid;
    void*        top_frame; // Python-only pre-unwind identity (NULL = no prior emit)
    unsigned int last_gen;
} thread_tracker_entry_t;

typedef struct {
    thread_tracker_entry_t entries[MAX_THREAD_TRACKER];
    size_t                 count;
    unsigned int           sample_gen;
} thread_tracker_t;

/**
 * Create a new thread tracker.
 *
 * @return a pointer to the newly created thread_tracker_t, or NULL on failure.
 */
thread_tracker_t*
thread_tracker_new();

/**
 * Return the existing entry for tid, or allocate and return a new one.
 * Returns NULL only if the tracker is full (> MAX_THREAD_TRACKER threads).
 */
thread_tracker_entry_t*
thread_tracker__get_or_create(thread_tracker_t*, uintptr_t tid);

/**
 * Remove entries that were not visited in the current sample generation.
 * Uses swap-with-last to keep the array compact.
 */
void
thread_tracker__evict_stale(thread_tracker_t*);

/**
 * Destroy the thread tracker and free all associated memory.
 */
void
thread_tracker__destroy(thread_tracker_t*);
