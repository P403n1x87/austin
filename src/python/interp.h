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

#ifndef PYTHON_INTERP_H
#define PYTHON_INTERP_H

#include <stdbool.h>
#include <stdint.h>

#include "gc.h"
#include "misc.h"

// ---- pystate.h -------------------------------------------------------------

struct _ts; /* Forward */

typedef struct _is2 {
    struct _is2 *next;
    struct _ts *tstate_head;
    void* gc;  /* Dummy */
} PyInterpreterState2;

// ---- internal/pycore_interp.h ----------------------------------------------

typedef void *PyThread_type_lock;

struct _pending_calls {
    PyThread_type_lock lock;
    _Py_atomic_int calls_to_do;
    int async_exc;
#define NPENDINGCALLS 32
    struct {
        int (*func)(void *);
        void *arg;
    } calls[NPENDINGCALLS];
    int first;
    int last;
};

struct _ceval_state {
    int recursion_limit;
    int tracing_possible;
    _Py_atomic_int eval_breaker;
    _Py_atomic_int gil_drop_request;
    struct _pending_calls pending;
};

typedef struct _is3_9 {

    struct _is3_9 *next;
    struct _ts *tstate_head;

    /* Reference to the _PyRuntime global variable. This field exists
       to not have to pass runtime in addition to tstate to a function.
       Get runtime from tstate: tstate->interp->runtime. */
    struct pyruntimestate *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    int finalizing;

    struct _ceval_state ceval;
    struct _gc_runtime_state3_8 gc;
} PyInterpreterState3_9;

typedef struct _is3_11 {

    struct _is3_11 *next;

    struct pythreads {
        uint64_t next_unique_id;
        struct _ts *head;  /* The linked list of threads, newest first. */
        long count;
        size_t stacksize;
    } threads;

    struct pyruntimestate3_11 *runtime;

    int64_t id;
    int64_t id_refcount;
    int requires_idref;
    PyThread_type_lock id_mutex;

    int _initialized;
    int finalizing;

    /* Was this interpreter statically allocated? */
    bool _static;

    struct _ceval_state ceval;
    struct _gc_runtime_state3_8 gc;
} PyInterpreterState3_11;



typedef union {
  PyInterpreterState2    v2;
  PyInterpreterState3_9  v3_9;
  PyInterpreterState3_11 v3_11;
} PyInterpreterState;

#endif