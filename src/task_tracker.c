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
    return self;
}

// ----------------------------------------------------------------------------
task_tracker_entry_t*
task_tracker__get_or_create(task_tracker_t* self, uintptr_t task) {
    for (size_t i = 0; i < self->count; i++) {
        if (self->entries[i].task == task)
            return &self->entries[i];
    }

    if (self->count >= MAX_TASK_TRACKER)
        return NULL;

    task_tracker_entry_t* entry = &self->entries[self->count++];
    memset(entry, 0, sizeof(*entry));
    entry->task = task;
    return entry;
}

// ----------------------------------------------------------------------------
void
task_tracker__remove_at(task_tracker_t* self, size_t i) {
    self->entries[i] = self->entries[--self->count];
}

// ----------------------------------------------------------------------------
void
task_tracker__destroy(task_tracker_t* self) {
    free(self);
}
