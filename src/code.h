// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#pragma once

#include "cache.h"
#include "py_proc.h"
#include "py_string.h"
#include "version.h"

typedef unsigned char* line_table_t;

typedef struct {
    key_dt           key;
    cached_string_t* filename;
    cached_string_t* scope;
    line_table_t     line_table;
    size_t           line_table_size;
    unsigned int     first_line_number;
} code_t;

// ----------------------------------------------------------------------------
static inline code_t*
code_new(
    key_dt key, cached_string_t* filename, cached_string_t* scope, line_table_t line_table, size_t line_table_size,
    unsigned int first_line_number
) {
    code_t* code = (code_t*)malloc(sizeof(code_t));
    if (!isvalid(code)) {
        return NULL;
    }

    code->key               = key;
    code->filename          = filename;
    code->scope             = scope;
    code->line_table        = line_table;
    code->line_table_size   = line_table_size;
    code->first_line_number = first_line_number;

    return code;
}

// ----------------------------------------------------------------------------
static inline void
code__destroy(code_t* self) {
    if (!isvalid(self)) {
        return;
    }

    sfree(self->line_table);

    free(self);
}

// ----------------------------------------------------------------------------

#define _code__get_filename(self, pref, py_v)                                         \
    _string_remote(pref, *((raddr_t*)((void*)self + py_v->py_code.o_filename)), py_v)

#define _code__get_name(self, pref, py_v) _string_remote(pref, *((raddr_t*)((void*)self + py_v->py_code.o_name)), py_v)

#define _code__get_qualname(self, pref, py_v)                                         \
    _string_remote(pref, *((raddr_t*)((void*)self + py_v->py_code.o_qualname)), py_v)

#define _code__get_lnotab(self, pref, len, py_v)                                        \
    _bytes_remote(pref, *((raddr_t*)((void*)self + py_v->py_code.o_lnotab)), len, py_v)

// ----------------------------------------------------------------------------
static inline code_t*
_code_remote(py_proc_t* py_proc, raddr_t code_raddr) {
    V_DESC(py_proc->py_v);

    proc_ref_t pref = py_proc->ref;

    V_ALLOCA(code, code);

    if (fail(copy_py(pref, code_raddr, py_code, code)))
        FAIL_PTR;

    lru_cache_t* cache = py_proc->string_cache;

    // Get the file name from the code object
    key_dt           string_key = py_string_key(code, o_filename);
    cached_string_t* filename   = (cached_string_t*)lru_cache__maybe_hit(cache, string_key);
    if (!isvalid(filename)) {
        char* filename_value = _code__get_filename(&code, pref, py_v);
        if (!isvalid(filename_value)) {
            FAIL_PTR;
        }
        filename = cached_string_new(string_key, filename_value);
        if (!isvalid(filename)) {
            FAIL_PTR;
        }
        lru_cache__store(cache, string_key, filename);

        event_handler__emit_new_string(filename);
    }

    // Get the function name from the code object
    string_key             = V_MIN(3, 11) ? py_string_key(code, o_qualname) : py_string_key(code, o_name);
    cached_string_t* scope = (cached_string_t*)lru_cache__maybe_hit(cache, string_key);
    if (!isvalid(scope)) {
        char* scope_value = V_MIN(3, 11) ? _code__get_qualname(&code, pref, py_v) : _code__get_name(&code, pref, py_v);
        if (!isvalid(scope_value)) {
            FAIL_PTR;
        }
        scope = cached_string_new(string_key, scope_value);
        if (!isvalid(scope)) {
            FAIL_PTR;
        }
        lru_cache__store(cache, string_key, scope);

        event_handler__emit_new_string(scope);
    }

    // Get the code location table from the code object
    ssize_t      len    = 0;
    line_table_t lnotab = _code__get_lnotab(&code, pref, &len, py_v);
    if (!isvalid(lnotab)) {
        FAIL_PTR;
    }

    return code_new(
        (key_dt)code_raddr, filename, scope, lnotab, len, V_FIELD(unsigned int, code, py_code, o_firstlineno)
    );
}
