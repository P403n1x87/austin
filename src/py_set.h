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

// Remote PySet read/iteration helpers.
//
// py_set__read     — read a PySetObject's used/mask/table triple.
// py_set__is_valid — sanity-check a just-read py_set_t before iterating.
// py_set__next     — advance a py_set_iter_t to the next live key.
//
// Iteration walks the raw hash table exactly as CPython's own setobject.c
// does, skipping empty (NULL-key) slots and stopping once every live entry
// (used) has been visited.
//
// copy_memory call budget:
//   1  py_set__read (used, mask, table read individually via copy_field_v)
//   1  per occupied-or-empty slot visited in py_set__next

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hints.h"
#include "mem.h"
#include "version.h"

// Whilst CPython has a limit of (1 << 20), we never expect to come across
// massive sets in practice, so we use a smaller values to reduce the chances
// of reading bogus memory.
#define MAX_SET_TABLE_SLOTS (1 << 16)

typedef struct {
    ssize_t used;
    ssize_t mask;
    raddr_t table;
} py_set_t;

// Cursor for py_set__next -- zero-initialise (e.g. `= {0}`) before the first
// call.
typedef struct {
    ssize_t slot;
    ssize_t seen;
} py_set_iter_t;

/**
 * Read a PySetObject's used/mask/table triple from remote memory.
 *
 * @param pref     the process reference.
 * @param set_addr the remote address of the PySetObject.
 * @param py_v     the version descriptor.
 * @param out      the py_set_t to fill.
 *
 * @return true on success, false on a failed remote read.
 */
static inline bool
py_set__read(proc_ref_t pref, raddr_t set_addr, python_v* py_v, py_set_t* out) {
    return success(copy_field_v(pref, set, used, set_addr, out->used))
        && success(copy_field_v(pref, set, mask, set_addr, out->mask))
        && success(copy_field_v(pref, set, table, set_addr, out->table));
}

/**
 * Sanity-check a just-read py_set_t before trusting it enough to iterate.
 *
 * Guards against corrupted/garbled remote memory -- the underlying set may
 * be concurrently mutated by the target process.
 *
 * @param set the py_set_t to check.
 *
 * @return true if set looks plausible.
 */
static inline bool
py_set__is_valid(const py_set_t* set) {
    return set->mask >= 0 && set->mask < MAX_SET_TABLE_SLOTS && set->used >= 0 && set->used <= set->mask + 1
        && isvalid(set->table);
}

/**
 * Advance it to the next live key in set.
 *
 * Reads one table slot at a time, skipping empty (NULL-key) slots, exactly
 * as CPython's own setobject.c iterates.
 *
 * @param pref    the process reference.
 * @param set     the py_set_t to iterate (see py_set__read).
 * @param it      the iterator cursor.
 * @param out_key set to the next live key on success.
 *
 * @return true with *out_key set on success; false once every live entry
 *         has been visited, or on a torn remote read (the two are
 *         indistinguishable to the caller -- both just mean "stop").
 */
static inline bool
py_set__next(proc_ref_t pref, const py_set_t* set, py_set_iter_t* it, raddr_t* out_key) {
    while (it->slot < set->mask + 1 && it->seen < set->used) {
        ssize_t slot = it->slot++;
        raddr_t key  = NULL;
        if (fail(copy_remote(pref, (char*)set->table + slot * (ssize_t)(2 * sizeof(void*)), key)))
            return false;
        if (isvalid(key)) {
            it->seen++;
            *out_key = key;
            return true;
        }
    }
    return false;
}
