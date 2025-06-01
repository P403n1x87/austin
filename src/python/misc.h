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

#pragma once

#include <stdint.h>

#include "../platform.h"
#include "code.h"

typedef struct _Py_atomic_address {
    uintptr_t _value;
} _Py_atomic_address;

#ifdef PL_UNIX
#include <pthread.h>
#define NATIVE_TSS_KEY_T pthread_key_t
#else
#define NATIVE_TSS_KEY_T unsigned long
#endif

struct _Py_tss_t {
    int              _is_initialized;
    NATIVE_TSS_KEY_T _key;
};

typedef struct _Py_tss_t Py_tss_t; /* opaque */

typedef struct _Py_atomic_int {
    int _value;
} _Py_atomic_int;

#ifdef PL_UNIX
#include <pthread.h>

#define PyMUTEX_T pthread_mutex_t
#define PyCOND_T  pthread_cond_t

#else
#include <windows.h>

typedef CRITICAL_SECTION PyMUTEX_T;
typedef struct _PyCOND_T {
    HANDLE sem;
    int    waiting;
} PyCOND_T;

#endif

#define COMMON_FIELDS(PREFIX)                                                 \
    PyObject* PREFIX##globals;                                                \
    PyObject* PREFIX##builtins;                                               \
    PyObject* PREFIX##name;                                                   \
    PyObject* PREFIX##qualname;                                               \
    PyObject* PREFIX##code;       /* A code object, the __code__ attribute */ \
    PyObject* PREFIX##defaults;   /* NULL or a tuple */                       \
    PyObject* PREFIX##kwdefaults; /* NULL or a dict */                        \
    PyObject* PREFIX##closure;    /* NULL or a tuple of cell objects */

typedef PyObject* (*vectorcallfunc)(PyObject* callable, PyObject* const* args, size_t nargsf, PyObject* kwnames);

typedef struct {
    PyObject_HEAD  COMMON_FIELDS(func_) PyObject* func_doc; /* The __doc__ attribute, can be anything */
    PyObject*      func_dict;                               /* The __dict__ attribute, a dict or NULL */
    PyObject*      func_weakreflist;                        /* List of weak references */
    PyObject*      func_module;                             /* The __module__ attribute, can be anything */
    PyObject*      func_annotations;                        /* Annotations, a dict or NULL */
    vectorcallfunc vectorcall;
    /* Version number for use by specializer.
     * Can set to non-zero when we want to specialize.
     * Will be set to zero if any of these change:
     *     defaults
     *     kwdefaults (only if the object changes, not the contents of the dict)
     *     code
     *     annotations */
    uint32_t       func_version;

    /* Invariant:
     *     func_closure contains the bindings for func_code->co_freevars, so
     *     PyTuple_Size(func_closure) == PyCode_GetNumFree(func_code)
     *     (func_closure may be NULL if PyCode_GetNumFree(func_code) == 0).
     */
} PyFunctionObject;

typedef uint16_t _Py_CODEUNIT;

struct _opaque {
    int            computed_line;
    const uint8_t* lo_next;
    const uint8_t* limit;
};

typedef struct _line_offsets {
    int            ar_start;
    int            ar_end;
    int            ar_line;
    struct _opaque opaque;
} PyCodeAddressRange;

typedef struct {
    PyCodeObject*      code;   // The code object for the bounds. May be NULL.
    PyCodeAddressRange bounds; // Only valid if code != NULL.
} PyTraceInfo;

typedef struct _stack_chunk {
    struct _stack_chunk* previous;
    size_t               size;
    size_t               top;
    PyObject*            data[1]; /* Variable sized */
} _PyStackChunk;
