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

// ---- Include/cpython/longintrepr.h -----------------------------------------
// CPython uses PYLONG_BITS_IN_DIGIT = 30 on 64-bit targets (digit = uint32_t)
// and PYLONG_BITS_IN_DIGIT = 15 on 32-bit targets (digit = uint16_t).
//
// Both shift values are exposed as named constants.  py_long__read() selects
// the correct one at runtime based on py_v->py_object_size so that a 64-bit
// Austin binary can correctly profile a 32-bit Python process.
#define PyLong_SHIFT_30 30
#define PyLong_SHIFT_15 15

// py_digit_t reflects the *host* build's digit type, used only for struct
// layout in PyLongBody3_11 / PyLongValue3_12.  py_long__read() does not use
// these structs directly — it reads into a raw buffer and interprets the bytes
// according to the runtime-detected target pointer size.
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ >= 8
typedef uint32_t py_digit_t;
#else
typedef uint16_t py_digit_t;
#endif

// ---- PyLongObject, 3.10-3.11 -----------------------------------------------
// From Include/cpython/longintrepr.h (3.11):
//   struct _longobject { PyObject_VAR_HEAD; digit ob_digit[1]; }
// PyObject_VAR_HEAD = PyObject_HEAD + Py_ssize_t ob_size.
// The body below is relative to the start of PyObject_VAR_HEAD's non-base part
// (i.e. ob_size + ob_digit[]).  Absolute offsets are py_object_size + offsetof.
typedef struct {
    Py_ssize_t ob_size;     // sign (< 0 = negative) and digit count (|ob_size|)
    py_digit_t ob_digit[2]; // 2 digits — we read at most 2 (thread idents fit in 2 × 30-bit digits)
} PyLongBody3_11;

// ---- PyLongObject, 3.12+ ----------------------------------------------------
// From Include/cpython/longintrepr.h (3.12):
//   typedef struct _PyLongValue { uintptr_t lv_tag; digit ob_digit[1]; } _PyLongValue;
//   struct _longobject { PyObject_HEAD; _PyLongValue long_value; }
//
// lv_tag encoding (from Include/cpython/longintrepr.h and pycore_long.h):
//
// Compact form: lv_tag < (2 << NON_SIZE_BITS)  i.e. lv_tag < 16
//   value = sign * ob_digit[0]
//   sign  = 1 - (lv_tag & SIGN_MASK):  &3==0 → +1,  &3==1 → 0 (zero),  &3==2 → -1
//
// Non-compact: lv_tag >= 16
//   digit_count = lv_tag >> NON_SIZE_BITS
//   negative    = (lv_tag & SIGN_MASK) == SIGN_NEGATIVE
//
// Constants from Include/cpython/longintrepr.h:
#define _PyLong_NON_SIZE_BITS 3
#define _PyLong_SIGN_MASK     3
#define _PyLong_SIGN_ZERO     1
#define _PyLong_SIGN_NEGATIVE 2

typedef struct {
    uintptr_t  lv_tag;
    py_digit_t ob_digit[2]; // 2 digits — we read at most 2 (thread idents fit in 2 × 30-bit digits)
} PyLongValue3_12;
