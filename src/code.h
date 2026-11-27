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

#include <string.h>

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
    raddr_t          co_tlbc_raddr; // cached _PyCodeArray* for TLBC (3.14+ FT); NULL otherwise
    raddr_t*         tlbc_entries;  // snapshot of _PyCodeArray.entries; NULL until first read
    ssize_t          tlbc_count;    // number of entries in the snapshot
    uint64_t         tlbc_gen;      // tlbc_generation at which the snapshot was taken
    uint64_t         ident;         // see _code__ident
    bool             is_volatile;   // see _code__is_volatile
} code_t;

// ----------------------------------------------------------------------------
// Identity of a code object (local copy), used to detect cache entries whose
// remote address has been recycled by a new code object. co_version (3.12+)
// is unique per code object; older versions rely on the other fields.
static inline uint64_t
_code__ident(python_v* py_v, void* code) {
    uint64_t ident = 0xcbf29ce484222325ULL;
#define _CODE_IDENT_MIX(v)         \
    {                              \
        ident ^= (uint64_t)(v);    \
        ident *= 0x100000001b3ULL; \
    }
    _CODE_IDENT_MIX(V_FIELD_PTR(uintptr_t, code, py_code, o_filename));
    _CODE_IDENT_MIX(V_FIELD_PTR(uintptr_t, code, py_code, o_name));
    _CODE_IDENT_MIX(V_FIELD_PTR(uintptr_t, code, py_code, o_lnotab));
    _CODE_IDENT_MIX(V_FIELD_PTR(unsigned int, code, py_code, o_firstlineno));
    if (V_MIN(3, 11))
        _CODE_IDENT_MIX(V_FIELD_PTR(uintptr_t, code, py_code, o_qualname));
    if (V_EQ(3, 12) || V_EQ(3, 13)) {
        // Not in the debug offsets: co_version precedes co_localsplusnames
        // and co_localspluskinds, which precede co_filename.
        offset_t o_version = py_v->py_code.o_filename - 2 * sizeof(void*) - sizeof(uint32_t);
        _CODE_IDENT_MIX(*(uint32_t*)((char*)code + o_version));
    }
#undef _CODE_IDENT_MIX
    return ident ? ident : 1; // 0 means "no identity"
}

// 3.14+ invalidates the caches on code object generation changes instead.
#define code_cache_needs_validation() (!V_MIN(3, 14))

#define CO_OPTIMIZED 0x0001

// ----------------------------------------------------------------------------
// Whether a code object (local copy) is likely to be freed after it has run,
// i.e. module and class bodies and exec'd code, which lack CO_OPTIMIZED. Only
// these are validated on cache hits, as checking every frame is too costly.
static inline bool
_code__is_volatile(python_v* py_v, void* code) {
    return code_cache_needs_validation() && py_v->py_code.o_flags
        && !(V_FIELD_PTR(int, code, py_code, o_flags) & CO_OPTIMIZED);
}

// ----------------------------------------------------------------------------
// Identity of the remote code object at code_raddr, or 0 on failure.
static inline uint64_t
_code__ident_remote(py_proc_t* py_proc, raddr_t code_raddr) {
    V_DESC(py_proc->py_v);

    V_ALLOCA(code, code);

    if (fail(copy_py(py_proc->ref, code_raddr, py_code, code)))
        return 0;

    return _code__ident(py_v, code);
}

// ----------------------------------------------------------------------------
static inline code_t*
code_new(
    key_dt key, cached_string_t* filename, cached_string_t* scope, line_table_t line_table, size_t line_table_size,
    unsigned int first_line_number, raddr_t co_tlbc_raddr
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
    code->co_tlbc_raddr     = co_tlbc_raddr;
    code->tlbc_entries      = NULL;
    code->tlbc_count        = 0;
    code->tlbc_gen          = 0;
    code->ident             = 0;
    code->is_volatile       = false;

    return code;
}

// ----------------------------------------------------------------------------
static inline void
code__destroy(code_t* self) {
    if (!isvalid(self)) {
        return;
    }

    sfree(self->line_table);
    sfree(self->tlbc_entries);

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
// Get the cached string for key, taking ownership of its freshly read value.
// Strings are keyed by address, which can be recycled, so a cached value that
// differs is superseded by a new entry, which is emitted. The old one is kept,
// as cached frames might still point to it.
static inline cached_string_t*
_code__cached_string(py_proc_t* py_proc, key_dt key, char* value) {
    lru_cache_t*     cache  = py_proc->string_cache;
    cached_string_t* string = (cached_string_t*)lru_cache__maybe_hit(cache, key);

    if (isvalid(string) && strcmp(string->value, value) == 0) {
        free(value);
        return string;
    }

    string = cached_string_new(key, value);
    if (!isvalid(string)) {
        free(value);
        FAIL_PTR;
    }
    lru_cache__store(cache, key, string);

    event_handler__emit_new_string(string);

    return string;
}

// ----------------------------------------------------------------------------
static inline code_t*
_code_remote(py_proc_t* py_proc, raddr_t code_raddr) {
    V_DESC(py_proc->py_v);

    proc_ref_t pref = py_proc->ref;

    V_ALLOCA(code, code);

    if (fail(copy_py(pref, code_raddr, py_code, code)))
        FAIL_PTR;

    // Get the file name from the code object
    char* filename_value = _code__get_filename(&code, pref, py_v);
    if (!isvalid(filename_value)) {
        FAIL_PTR;
    }
    cached_string_t* filename = _code__cached_string(py_proc, py_string_key(code, o_filename), filename_value);
    if (!isvalid(filename)) {
        FAIL_PTR;
    }

    // Get the function name from the code object
    char* scope_value = V_MIN(3, 11) ? _code__get_qualname(&code, pref, py_v) : _code__get_name(&code, pref, py_v);
    if (!isvalid(scope_value)) {
        FAIL_PTR;
    }
    cached_string_t* scope = _code__cached_string(
        py_proc, V_MIN(3, 11) ? py_string_key(code, o_qualname) : py_string_key(code, o_name), scope_value
    );
    if (!isvalid(scope)) {
        FAIL_PTR;
    }

    // Get the code location table from the code object
    ssize_t      len    = 0;
    line_table_t lnotab = _code__get_lnotab(&code, pref, &len, py_v);
    if (!isvalid(lnotab)) {
        FAIL_PTR;
    }

    raddr_t co_tlbc_raddr = py_v->py_code.o_tlbc ? V_FIELD(raddr_t, code, py_code, o_tlbc) : NULL;

    code_t* result = code_new(
        (key_dt)code_raddr, filename, scope, lnotab, len, V_FIELD(unsigned int, code, py_code, o_firstlineno),
        co_tlbc_raddr
    );
    if (isvalid(result)) {
        result->ident       = _code__ident(py_v, code);
        result->is_volatile = _code__is_volatile(py_v, code);
    }

    return result;
}
