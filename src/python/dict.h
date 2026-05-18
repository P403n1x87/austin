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
//
// COPYRIGHT NOTICE: The content of this file is composed of different parts
//                   taken from different versions of the source code of
//                   Python. The authors of those sources hold the copyright
//                   for most of the content of this header file.

#pragma once

#include <stdint.h>

#include "object.h"

// ---- Objects/dict-common.h (3.10-3.11) / Include/internal/pycore_dict.h ----

// PyDictKeyEntry is used for all dict entries in 3.10-3.11, and for general
// (non-unicode-keyed) dicts in 3.12+.
typedef struct {
    Py_hash_t me_hash;  // 8 bytes: cached hash of me_key
    PyObject* me_key;   // 8 bytes
    PyObject* me_value; // 8 bytes (NULL in split tables)
} PyDictKeyEntry;

// PyDictUnicodeEntry is used in 3.12+ for unicode-keyed (compact) dicts.
// Module __dict__ and sys.modules always use this form.
typedef struct {
    PyObject* me_key;   // 8 bytes (unicode, pre-hashed)
    PyObject* me_value; // 8 bytes
} PyDictUnicodeEntry;

// DictKeysKind values (3.12+)
#define DICT_KEYS_GENERAL 0
#define DICT_KEYS_UNICODE 1
#define DICT_KEYS_SPLIT   2

// ---- PyDictKeysObject, 3.10-3.11 -------------------------------------------
// From Objects/dict-common.h
typedef struct {
    Py_ssize_t dk_refcnt;
    Py_ssize_t dk_size;   // number of hash-table slots (power of 2)
    void*      dk_lookup; // function pointer, ignored
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries; // number of used (non-dummy) entries
    // char dk_indices[] follows — size = dk_size * index_entry_size
    // PyDictKeyEntry[] follows after the indices
} PyDictKeysObject3_10;

// ---- PyDictKeysObject, 3.11+ ------------------------------------------------
// This compact layout was introduced in CPython 3.11 (not 3.12).
// From Include/internal/pycore_dict.h
typedef struct {
    Py_ssize_t dk_refcnt;
    uint8_t    dk_log2_size;        // log2 of hash-table slot count
    uint8_t    dk_log2_index_bytes; // log2 of bytes per index slot
    uint8_t    dk_kind;             // DictKeysKind (0=general, 1=unicode, 2=split)
    uint8_t    _pad;
    uint32_t   dk_version;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    // char dk_indices[] follows — size = (1<<dk_log2_size) << dk_log2_index_bytes
    // PyDictKeyEntry[] or PyDictUnicodeEntry[] follows after the indices
} PyDictKeysObject3_11;

// Alias for readability at call sites that think of this as "3.12+"
typedef PyDictKeysObject3_11 PyDictKeysObject3_12;

// ---- PyDictValues, 3.13+ ---------------------------------------------------
// From Include/internal/pycore_dict.h (introduced in 3.13):
//   struct _dictvalues { uint8_t capacity; uint8_t size; uint8_t embedded;
//                        uint8_t valid; PyObject *values[1]; }
// For inline-values objects (TPFLAGS_INLINE_VALUES), the values array starts
// at offsetof(PyDictValues3_13, values[0]) = 8 (after 4 uint8 + 4-byte padding).
// In Python 3.11-3.12, PyDictValues was just { PyObject *values[1] } so
// values start at offset 0.
typedef struct {
    uint8_t   capacity;
    uint8_t   size;
    uint8_t   embedded;
    uint8_t   valid;
    // 4 bytes implicit padding for PyObject* alignment
    PyObject* values[1]; // at offsetof = 8
} PyDictValues3_13;

// ---- PyDictObject (all versions) -------------------------------------------
// The fields after PyObject_HEAD are stable across 3.10-3.14.
// PyObject_HEAD itself varies in size (GIL vs free-threaded), so we express
// the body separately and compute absolute offsets as py_object_size + offsetof.
typedef struct {
    Py_ssize_t ma_used;
    uint64_t   ma_version_tag;
    void*      ma_keys;   // PyDictKeysObject*
    void*      ma_values; // NULL for combined (non-split) dicts
} PyDictBody;
