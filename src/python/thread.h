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

#ifndef PYTHON_THREAD_H
#define PYTHON_THREAD_H

#include <stdint.h>

#include "cframe.h"
#include "frame.h"
#include "object.h"

// Dummy struct _frame
struct _frame;

typedef int (*Py_tracefunc)(PyObject *, struct _frame *, int, PyObject *);


typedef struct _err_stackitem3_7 {
    PyObject *exc_type, *exc_value, *exc_traceback;
    struct _err_stackitem3_7 *previous_item;
} _PyErr_StackItem3_7;


typedef struct _ts3_8 {
    struct _ts *prev;
    struct _ts *next;
    void *interp;
    PyFrameObject *frame;
    int recursion_depth;
    int recursion_headroom; /* Allow 50 more calls to handle any errors. */
    int stackcheck_counter;
    int tracing;
    int use_tracing;
    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;
    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;
    _PyErr_StackItem3_7 exc_state;
    _PyErr_StackItem3_7 *exc_info;
    PyObject *dict;  /* Stores per-thread state */
    int gilstate_counter;
    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */
    int trash_delete_nesting;
    PyObject *trash_delete_later;
    void (*on_delete)(void *);
    void *on_delete_data;
    int coroutine_origin_tracking_depth;
    PyObject *async_gen_firstiter;
    PyObject *async_gen_finalizer;
    PyObject *context;
    uint64_t context_ver;
    uint64_t id;
} PyThreadState3_8;


typedef struct _err_stackitem3_11 {
    PyObject *exc_value;
    struct _err_stackitem3_11 *previous_item;
} _PyErr_StackItem3_11;

typedef struct _ts3_11 {
    struct _ts3_11 *prev;
    struct _ts3_11 *next;
    void *interp;
    int _initialized;
    int _static;

    int recursion_remaining;
    int recursion_limit;
    int recursion_headroom; /* Allow 50 more calls to handle any errors. */

    int tracing;
    int tracing_what; /* The event currently being traced, if any. */

    /* Pointer to current _PyCFrame in the C stack frame of the currently,
     * or most recently, executing _PyEval_EvalFrameDefault. */
    void *cframe;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    PyObject *curexc_type;
    PyObject *curexc_value;
    PyObject *curexc_traceback;

    _PyErr_StackItem3_11 *exc_info;

    PyObject *dict;  /* Stores per-thread state */

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */
    unsigned long native_thread_id;

    int trash_delete_nesting;
    PyObject *trash_delete_later;

    void (*on_delete)(void *);
    void *on_delete_data;

    int coroutine_origin_tracking_depth;

    PyObject *async_gen_firstiter;
    PyObject *async_gen_finalizer;

    PyObject *context;
    uint64_t context_ver;

    uint64_t id;  /* Unique thread state id. */

    PyTraceInfo trace_info;

    _PyStackChunk *datastack_chunk;
    PyObject **datastack_top;
    PyObject **datastack_limit;
} PyThreadState3_11;


typedef struct {
    /* Has been initialized to a safe state.

        In order to be effective, this must be set to 0 during or right
        after allocation. */
    unsigned int initialized:1;

    /* Has been bound to an OS thread. */
    unsigned int bound:1;
    /* Has been unbound from its OS thread. */
    unsigned int unbound:1;
    /* Has been bound aa current for the GILState API. */
    unsigned int bound_gilstate:1;
    /* Currently in use (maybe holds the GIL). */
    unsigned int active:1;

    /* various stages of finalization */
    unsigned int finalizing:1;
    unsigned int cleared:1;
    unsigned int finalized:1;

    /* padding to align to 4 bytes */
    unsigned int :24;
} tstate_status_t;

typedef struct {
    /* See Python/ceval.c for comments explaining most fields */

    void *prev;
    void *next;
    void *interp;

    tstate_status_t _status;

    int py_recursion_remaining;
    int py_recursion_limit;

    int c_recursion_remaining;
    int recursion_headroom; /* Allow 50 more calls to handle any errors. */

    /* 'tracing' keeps track of the execution depth when tracing/profiling.
       This is to prevent the actual trace/profile code from being recorded in
       the trace/profile. */
    int tracing;
    int what_event; /* The event currently being monitored, if any. */

    /* Pointer to current _PyCFrame in the C stack frame of the currently,
     * or most recently, executing _PyEval_EvalFrameDefault. */
    void *cframe;

    Py_tracefunc c_profilefunc;
    Py_tracefunc c_tracefunc;
    PyObject *c_profileobj;
    PyObject *c_traceobj;

    /* The exception currently being raised */
    PyObject *current_exception;

    /* Pointer to the top of the exception stack for the exceptions
     * we may be currently handling.  (See _PyErr_StackItem above.)
     * This is never NULL. */
    void *exc_info;

    PyObject *dict;  /* Stores per-thread state */

    int gilstate_counter;

    PyObject *async_exc; /* Asynchronous exception to raise */
    unsigned long thread_id; /* Thread id where this tstate was created */

    unsigned long native_thread_id;

    struct {
        int delete_nesting;
        PyObject *delete_later;
    } trash;

    void (*on_delete)(void *);
    void *on_delete_data;

    int coroutine_origin_tracking_depth;

    PyObject *async_gen_firstiter;
    PyObject *async_gen_finalizer;

    PyObject *context;
    uint64_t context_ver;

    /* Unique thread state id. */
    uint64_t id;

    void *datastack_chunk;
    PyObject **datastack_top;
    PyObject **datastack_limit;
} PyThreadState3_12;


typedef union {
  PyThreadState3_8  v3_8;
  PyThreadState3_11 v3_11;
  PyThreadState3_12 v3_12;
} PyThreadState;

#endif
