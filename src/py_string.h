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

// ----------------------------------------------------------------------------
typedef struct _string {
    key_dt key;
    char*  value;
} cached_string_t;

static cached_string_t _unknown_scope __attribute__((unused)) = {.key = 1, .value = "<unknown>"};
#define UNKNOWN_SCOPE (&_unknown_scope)

static inline bool
is_pyeval_frame(cached_string_t* scope) {
    if (scope == UNKNOWN_SCOPE || !isvalid(scope->value))
        return false;
    // Traditional interpreter: _PyEval_EvalFrameDefault appears on the stack.
    // Tail-call interpreter (CPython 3.15+ with Clang): _PyEval_EvalFrameDefault
    // is tail-called away; opcode handlers (_TAIL_CALL_<OPCODE>) appear instead.
    // Anchor to the start of the name to avoid false positives.
    return isvalid(strstr(scope->value, "PyEval_EvalFrameDefault")) || strncmp(scope->value, "_TAIL_CALL_", 11) == 0;
}

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
    char*   buffer = NULL;
    ssize_t len    = 0;

    // For Python 3.13+ (GIL and free-threaded) use the version-descriptor
    // unicode offsets to locate the state bitfield, length, and ASCII data. The
    // PyObject header is larger in free-threaded builds.
    if (V_MIN(3, 13) && py_v->py_unicode.ascii_size > 0) {
        // Read length[8] + hash[8] + state[8]. The state struct is 8 bytes on
        // all platforms (4-byte-aligned, padded to 8), but the bitfield layout
        // within it differs by compiler.
        char hdr[sizeof(ssize_t) + sizeof(Py_hash_t) + 2 * sizeof(uint32_t)];
        if (fail(copy_memory(pref, raddr + py_v->py_unicode.o_length, sizeof(hdr), hdr))) // GCOV_EXCL_LINE
            FAIL_PTR;                                                                     // GCOV_EXCL_LINE

        len                 = *(ssize_t*)(hdr);
        uint32_t state_word = *(uint32_t*)(hdr + sizeof(ssize_t) + sizeof(Py_hash_t));

        unsigned int kind, compact;
        if (py_v->py_unicode.ft_interned_byte) {
            // 3.14+ free-threaded: interned is a full unsigned char at byte +0
            // of the state struct. The compiler determines where
            // kind:3/compact:1 follows: GCC/Clang packs them at byte +1; MSVC
            // aligns unsigned int bitfields to a 4-byte boundary so they land
            // at byte +4. Probe on first use: try +1 (GCC), fall back to +4
            // (MSVC).
            if (unlikely(py_v->py_unicode.ft_state_kind_byte < 0)) {
                // Probe: a valid kind is 1–4; if byte +1 gives that, it's GCC
                // layout.
                int kb1                             = *(uint8_t*)(hdr + sizeof(ssize_t) + sizeof(Py_hash_t) + 1) & 7;
                int kb4                             = *(uint8_t*)(hdr + sizeof(ssize_t) + sizeof(Py_hash_t) + 4) & 7;
                py_v->py_unicode.ft_state_kind_byte = (kb1 >= 1 && kb1 <= 4) ? 1 : (kb4 >= 1 && kb4 <= 4) ? 4 : 1;
                log_d("Probed FT unicode state kind byte at +%d", py_v->py_unicode.ft_state_kind_byte);
            }
            uint8_t bits = *(uint8_t*)(hdr + sizeof(ssize_t) + sizeof(Py_hash_t) + py_v->py_unicode.ft_state_kind_byte);
            kind         = bits & 7;
            compact      = (bits >> 3) & 1;
        } else {
            // GIL builds and 3.13 free-threaded: classic 2-bit interned bitfield.
            kind    = (state_word >> 2) & 7;
            compact = (state_word >> 5) & 1;
        }

        if (kind != 1) { // GCOV_EXCL_START
            set_error(PYOBJECT, "Invalid PyASCIIObject kind");
            FAIL_PTR;
        } // GCOV_EXCL_STOP

        // Buffer for non-compact path: utf8_length[8] + utf8 pointer[8].
        char    utf8_hdr[sizeof(ssize_t) + sizeof(raddr_t)];
        raddr_t data;
        if (compact) { // GCOV_EXCL_BR_LINE
            data = p_ascii_data(raddr, py_v->py_unicode.ascii_size);
        } else { // GCOV_EXCL_START
            if (fail(
                    copy_memory(pref, raddr + py_v->py_unicode.ascii_size + sizeof(ssize_t), sizeof(utf8_hdr), utf8_hdr)
                ))
                FAIL_PTR;
            len  = *(ssize_t*)utf8_hdr;
            data = *(raddr_t*)(utf8_hdr + sizeof(ssize_t));
        } // GCOV_EXCL_STOP

        if (!isvalid(data)) { // GCOV_EXCL_START
            set_error(PYOBJECT, "Invalid PyASCIIObject data pointer");
            FAIL_PTR;
        } // GCOV_EXCL_STOP

        if (len < 0 || len > 4096) { // GCOV_EXCL_START
            set_error(PYOBJECT, "Invalid string length");
            FAIL_PTR;
        } // GCOV_EXCL_STOP

        buffer = (char*)malloc(len + 1);
        if (!isvalid(buffer)) { // GCOV_EXCL_START
            set_error(MALLOC, "Cannot allocate memory for string buffer");
            FAIL_PTR;
        } // GCOV_EXCL_STOP

        if (fail(copy_memory(pref, data, len, buffer))) { // GCOV_EXCL_START
            free(buffer);
            FAIL_PTR;
        } // GCOV_EXCL_STOP

        buffer[len] = '\0';
        return buffer;
    }

    // Legacy path for Python < 3.13: use hardcoded GIL struct layout.
    PyUnicodeObject unicode;
    if (fail(copy_datatype(pref, raddr, unicode)))
        FAIL_PTR;

    PyASCIIObject ascii = unicode.v3._base._base;

    if (ascii.state.kind != 1) { // GCOV_EXCL_START
        set_error(PYOBJECT, "Invalid PyASCIIObject kind");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    ssize_t ascii_size = V_MIN(3, 12) ? sizeof(unicode.v3_12._base._base) : sizeof(unicode.v3._base._base);
    raddr_t data       = ascii.state.compact ? p_ascii_data(raddr, ascii_size)
                                             : (V_MIN(3, 12) ? unicode.v3_12._base.utf8 : unicode.v3._base.utf8);
    len = ascii.state.compact ? ascii.length
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

    buffer[len] = '\0';
    return buffer;
}

// ----------------------------------------------------------------------------
static inline unsigned char*
_bytes_remote(proc_ref_t pref, raddr_t raddr, ssize_t* size, python_v* py_v) {
    ssize_t        len   = 0;
    unsigned char* array = NULL;

    // ob_size is at offset py_object_size (right after the PyObject header).
    if (fail(copy_memory(pref, raddr + py_v->py_object_size, sizeof(ssize_t), &len))) // GCOV_EXCL_LINE
        FAIL_PTR;                                                                     // GCOV_EXCL_LINE
    len++;                                                                            // Include null-terminator

    if (len < 1) { // GCOV_EXCL_START
        set_error(PYOBJECT, "PyBytesObject is too short");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (len > (100 << 20)) { // GCOV_EXCL_START
        set_error(PYOBJECT, "PyBytesObject size too big to be valid");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    array = (unsigned char*)malloc((len + 1) * sizeof(unsigned char*));
    if (!isvalid(array)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for PyBytesObject buffer");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    // ob_sval is at py_object_size + ob_size[8] + ob_shash[8] = py_object_size + 16
    if (fail(copy_memory(pref, raddr + py_v->py_object_size + 16, len, array))) { // GCOV_EXCL_START
        free(array);
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    array[len] = 0;
    *size      = len - 1;

    return array;
}

#define py_string_key(code, field) ((key_dt) * ((raddr_t*)((raddr_t) & code + py_v->py_code.field)))
