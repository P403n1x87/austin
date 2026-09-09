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

#include <stdlib.h>
#include <string.h>

#include "task_tracker.h"

// ----------------------------------------------------------------------------
task_tracker_t*
task_tracker_new(void) {
    task_tracker_t* self = calloc(1, sizeof(task_tracker_t));
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return NULL;    // GCOV_EXCL_LINE

    // Small initial size: lookup_t grows itself (doubling) as needed, and
    // most processes never come close to MAX_TASK_TRACKER concurrently.
    self->by_task = lookup_new(256);
    if (!isvalid(self->by_task)) { // GCOV_EXCL_START
        free(self);
        return NULL;
    } // GCOV_EXCL_STOP

    return self;
}

// ----------------------------------------------------------------------------
task_tracker_entry_t*
task_tracker__get_or_create(task_tracker_t* self, uintptr_t task) {
    task_tracker_entry_t* entry = (task_tracker_entry_t*)lookup__get(self->by_task, (key_dt)task);
    if (isvalid(entry))
        return entry;

    if (self->count >= MAX_TASK_TRACKER)
        return NULL;

    entry = (task_tracker_entry_t*)calloc(1, sizeof(task_tracker_entry_t));
    if (!isvalid(entry)) // GCOV_EXCL_LINE
        return NULL;     // GCOV_EXCL_LINE

    entry->task = task;
    lookup__set(self->by_task, (key_dt)task, (value_t)entry);
    self->count++;
    return entry;
}

// ----------------------------------------------------------------------------
size_t
task_tracker__collect_stale(task_tracker_t* self, task_tracker_entry_t** out, size_t max) {
    size_t n = 0;

    hash_table__iter_start(self->by_task->hash, task_tracker_entry_t*, entry) {
        if (n >= max)
            break;
        if (entry->last_gen < self->sample_gen)
            out[n++] = entry;
    }
    hash_table__iter_stop(self->by_task->hash);

    return n;
}

// ----------------------------------------------------------------------------
void
task_tracker__remove(task_tracker_t* self, uintptr_t task) {
    task_tracker_entry_t* entry = (task_tracker_entry_t*)lookup__get(self->by_task, (key_dt)task);
    if (!isvalid(entry))
        return;

    lookup__del(self->by_task, (key_dt)task);
    free(entry);
    self->count--;
}

// ----------------------------------------------------------------------------
void
task_tracker__destroy(task_tracker_t* self) {
    if (!isvalid(self)) // GCOV_EXCL_LINE
        return;         // GCOV_EXCL_LINE

    // lookup_t takes no ownership of its values -- free every still-tracked
    // entry's own heap allocation before tearing down the table itself.
    hash_table__iter_start(self->by_task->hash, task_tracker_entry_t*, entry) { free(entry); }
    hash_table__iter_stop(self->by_task->hash);

    lookup__destroy(self->by_task);
    free(self);
}
