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

#include "thread_tracker.h"

// ----------------------------------------------------------------------------
thread_tracker_t*
thread_tracker_new(void) {
    thread_tracker_t* self = calloc(1, sizeof(thread_tracker_t));
    return self;
}

// ----------------------------------------------------------------------------
thread_tracker_entry_t*
thread_tracker__get_or_create(thread_tracker_t* self, uintptr_t tid) {
    for (size_t i = 0; i < self->count; i++) {
        if (self->entries[i].tid == tid)
            return &self->entries[i];
    }

    if (self->count >= MAX_THREAD_TRACKER)
        return NULL;

    thread_tracker_entry_t* entry = &self->entries[self->count++];
    memset(entry, 0, sizeof(*entry));
    entry->tid = tid;
    return entry;
}

// ----------------------------------------------------------------------------
void
thread_tracker__evict_stale(thread_tracker_t* self) {
    size_t i = 0;
    while (i < self->count) {
        if (self->entries[i].last_gen < self->sample_gen) {
            self->entries[i] = self->entries[--self->count];
        } else {
            i++;
        }
    }
}

// ----------------------------------------------------------------------------
void
thread_tracker__destroy(thread_tracker_t* self) {
    free(self);
}
