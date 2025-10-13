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

#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "error.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "platform.h"
#include "python/string.h"
#include "version.h"

#define MAGIC_TINY                7
#define MAGIC_BIG                 1000003
#define p_ascii_data(raddr, size) (raddr + size)

#define UNKNOWN_SCOPE ((cached_string_t*)1)

// ----------------------------------------------------------------------------
typedef struct _string {
    key_dt key;
    char*  value;
} cached_string_t;

static inline cached_string_t*
cached_string_new(key_dt key, char* value) {
    cached_string_t* cached_string = (cached_string_t*)malloc(sizeof(cached_string_t));
    if (!isvalid(cached_string)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for cached string");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    cached_string->key   = key;
    cached_string->value = value;

    return cached_string;
}

static inline void
cached_string_destroy(cached_string_t* cached_string) {
    if (isvalid(cached_string)) {
        sfree(cached_string->value);
        sfree(cached_string);
    }
}

// ----------------------------------------------------------------------------
static inline long
string__hash(char* string) {
    // Stolen from stringobject.c
    register unsigned char* p;
    register long           x;

    p = (unsigned char*)string;
    x = *p << MAGIC_TINY;
    while (*p != 0)
        x = (MAGIC_BIG * x) ^ *(p++);
    x ^= strlen(string);
    return x == 0 ? 1 : x;
}

// ----------------------------------------------------------------------------
static inline char*
_string_remote(proc_ref_t pref, raddr_t raddr, python_v* py_v) {
    PyUnicodeObject unicode;
    char*           buffer = NULL;
    ssize_t         len    = 0;

    if (fail(copy_datatype(pref, raddr, unicode)))
        FAIL_PTR;

    PyASCIIObject ascii = unicode.v3._base._base;

    if (ascii.state.kind != 1) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Invalid PyASCIIObject kind");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    // Because changes to PyASCIIObject are rare, we handle the version manually
    // instead of using a version offset descriptor.
    ssize_t ascii_size = V_MIN(3, 12) ? sizeof(unicode.v3_12._base._base) : sizeof(unicode.v3._base._base);
    raddr_t data       = ascii.state.compact ? p_ascii_data(raddr, ascii_size)
                                             : (V_MIN(3, 12) ? unicode.v3_12._base.utf8 : unicode.v3._base.utf8);
    len                = ascii.state.compact ? ascii.length
                                             : (V_MIN(3, 12) ? unicode.v3_12._base.utf8_length : unicode.v3._base.utf8_length);

    if (!isvalid(data)) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Invalid PyASCIIObject data pointer");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (len < 0 || len > 4096) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Invalid string length");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    buffer = (char*)malloc(len + 1); // GCOV_EXCL_START
    if (!isvalid(buffer)) {
        set_error(MALLOC, "Cannot allocate memory for string buffer");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (fail(copy_memory(pref, data, len, buffer))) { // GCOV_EXCL_START
        free(buffer);
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    buffer[len] = '\0'; // Ensure null-termination

    return buffer;
}

// ----------------------------------------------------------------------------
static inline unsigned char*
_bytes_remote(proc_ref_t pref, raddr_t raddr, ssize_t* size, python_v* py_v) {
    PyBytesObject  bytes = {0};
    ssize_t        len   = 0;
    unsigned char* array = NULL;

    if (fail(copy_datatype(pref, raddr, bytes))) // GCOV_EXCL_LINE
        FAIL_PTR;                                // GCOV_EXCL_LINE

    if ((len = bytes.ob_base.ob_size + 1) < 1) { // Include null-terminator // GCOV_EXCL_START
        set_error(PYOBJECT, "PyBytesObject is too short");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (len > (100 << 20)) { // 100MB
        set_error(PYOBJECT, "PyBytesObject size too big to be valid");
        FAIL_PTR;
    }

    array = (unsigned char*)malloc((len + 1) * sizeof(unsigned char*));
    if (!isvalid(array)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for PyBytesObject buffer");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (fail(copy_memory(pref, raddr + offsetof(PyBytesObject, ob_sval), len, array))) {
        free(array);
        FAIL_PTR;
    }

    array[len] = 0;
    *size      = len - 1;

    return array;
}

#define py_string_key(code, field) ((key_dt) * ((raddr_t*)((raddr_t) & code + py_v->py_code.field)))
