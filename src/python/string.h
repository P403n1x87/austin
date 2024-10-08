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
//
// COPYRIGHT NOTICE: The content of this file is composed of different parts
//                   taken from different versions of the source code of
//                   Python. The authors of those sources hold the copyright
//                   for most of the content of this header file.

#ifndef PYTHON_STRING_H
#define PYTHON_STRING_H

#include <stdint.h>

#include "object.h"

// ---- unicodeobject.h -------------------------------------------------------

typedef uint32_t Py_UCS4;
typedef uint16_t Py_UCS2;
typedef uint8_t Py_UCS1;

#define PY_UNICODE_TYPE Py_UCS4

typedef PY_UNICODE_TYPE Py_UNICODE;

typedef Py_ssize_t Py_hash_t;

typedef struct {
    PyObject_HEAD
    Py_ssize_t length;          /* Number of code points in the string */
    Py_hash_t hash;             /* Hash value; -1 if not set */
    struct {
        unsigned int interned:2;
        unsigned int kind:3;
        unsigned int compact:1;
        unsigned int ascii:1;
        unsigned int ready:1;
        unsigned int :24;
    } state;
    wchar_t *wstr;              /* wchar_t representation (null-terminated) */
} PyASCIIObject;

typedef struct {
    PyASCIIObject _base;
    Py_ssize_t utf8_length;     /* Number of bytes in utf8, excluding the
                                 * terminating \0. */
    char *utf8;                 /* UTF-8 representation (null-terminated) */
    Py_ssize_t wstr_length;     /* Number of code points in wstr, possible
                                 * surrogates count as two code points. */
} PyCompactUnicodeObject;


typedef struct {
    PyCompactUnicodeObject _base;
    union {
        void *any;
        Py_UCS1 *latin1;
        Py_UCS2 *ucs2;
        Py_UCS4 *ucs4;
    } data;                     /* Canonical, smallest-form Unicode buffer */
} PyUnicodeObject3;


typedef struct {
    struct {
        struct {
            PyObject_HEAD
            Py_ssize_t length;          /* Number of code points in the string */
            Py_hash_t hash;             /* Hash value; -1 if not set */
            struct {
                unsigned int interned:2;
                unsigned int kind:3;
                unsigned int compact:1;
                unsigned int ascii:1;
                unsigned int :25;
            } state;
        } _base;
        Py_ssize_t utf8_length;     /* Number of bytes in utf8, excluding the
                                    * terminating \0. */
        char *utf8;                 /* UTF-8 representation (null-terminated) */
    } _base;
    union {
        void *any;
        void *latin1;
        void *ucs2;
        void *ucs4;
    } data;                     /* Canonical, smallest-form Unicode buffer */
} PyUnicodeObject3_12;


typedef union {
    PyUnicodeObject3    v3;
    PyUnicodeObject3_12 v3_12;
} PyUnicodeObject;

// ---- bytesobject.h ---------------------------------------------------------

typedef struct {
    PyObject_VAR_HEAD
    Py_hash_t ob_shash;
    char ob_sval[1];
} PyBytesObject;

#endif
