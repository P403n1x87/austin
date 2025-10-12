// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#include <stdint.h>
#include <stdlib.h>

#include "cache.h"
#include "error.h"
#include "hints.h"

// We don't expect to have more than 256 concurrent interpreters. If we do, we
// might end up evicting interpreter states and losing tracking information
// about currently running ones. As a result, the frame cache might become
// stale.
#define MAX_INTERPRETER_STATE_CACHE_SIZE 256

typedef struct _interpreter_state {
    int64_t  id;
    uint64_t code_object_gen;
} interpreter_state_t;

// ----------------------------------------------------------------------------
static inline interpreter_state_t*
interpreter_state_new(int64_t id, uint64_t code_object_gen) {
    interpreter_state_t* state = (interpreter_state_t*)malloc(sizeof(interpreter_state_t));
    if (!isvalid(state)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate interpreter state structure");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    state->id              = id;
    state->code_object_gen = code_object_gen;

    return state;
}

// ----------------------------------------------------------------------------
static inline key_dt
interpreter_state_key(int64_t interp_id) {
    return (key_dt)(interp_id + 1); // Use interp_id + 1 to avoid confusion with NULL
}

// ----------------------------------------------------------------------------
static inline void
interpreter_state__destroy(interpreter_state_t* state) {
    if (!isvalid(state)) // GCOV_EXCL_LINE
        FAIL_VOID;       // GCOV_EXCL_LINE

    free(state);
}
