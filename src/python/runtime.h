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

#ifndef PYTHON_RUNTIME_H
#define PYTHON_RUNTIME_H

#include "ceval.h"
#include "gil.h"
#include "interp.h"
#include "misc.h"
#include "thread.h"

// ---- internal/pycore_pystate.h ---------------------------------------------

typedef struct pyruntimestate3_8 {
    int preinitializing;
    int preinitialized;
    int core_initialized;
    int initialized;
    PyThreadState *finalizing;

    struct pyinterpreters3_8 {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        int64_t next_id;
    } interpreters;
    // XXX Remove this field once we have a tp_* slot.
    struct _xidregistry3_8 {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _gc_runtime_state3_8 gc;
    // struct _ceval_runtime_state ceval;
    // struct _gilstate_runtime_state gilstate;
} _PyRuntimeState3_8;


// ---- internal/pycore_runtime.h ---------------------------------------------


typedef struct pyruntimestate3_11 {
    int _initialized;
    int preinitializing;
    int preinitialized;
    int core_initialized;
    int initialized;
    _Py_atomic_address _finalizing;

    struct pyinterpreters3_11 {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        int64_t next_id;
    } interpreters;

    struct _xidregistry3_11 {
        PyThread_type_lock mutex;
        struct _xidregitem *head;
    } xidregistry;

    unsigned long main_thread;

#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _ceval_runtime_state3_11 ceval;
    struct _gilstate_runtime_state3_11 gilstate;
} _PyRuntimeState3_11;


typedef struct pyruntimestate3_12 {
    /* Has been initialized to a safe state.

       In order to be effective, this must be set to 0 during or right
       after allocation. */
    int _initialized;

    /* Is running Py_PreInitialize()? */
    int preinitializing;

    /* Is Python preinitialized? Set to 1 by Py_PreInitialize() */
    int preinitialized;

    /* Is Python core initialized? Set to 1 by _Py_InitializeCore() */
    int core_initialized;

    /* Is Python fully initialized? Set to 1 by Py_Initialize() */
    int initialized;

    /* Set by Py_FinalizeEx(). Only reset to NULL if Py_Initialize()
       is called again.

       Use _PyRuntimeState_GetFinalizing() and _PyRuntimeState_SetFinalizing()
       to access it, don't access it directly. */
    _Py_atomic_address _finalizing;

    struct {
        PyThread_type_lock mutex;
        /* The linked list of interpreters, newest first. */
        void *head;
        /* The runtime's initial interpreter, which has a special role
           in the operation of the runtime.  It is also often the only
           interpreter. */
        void *main;
        /* next_id is an auto-numbered sequence of small
           integers.  It gets initialized in _PyInterpreterState_Enable(),
           which is called in Py_Initialize(), and used in
           PyInterpreterState_New().  A negative interpreter ID
           indicates an error occurred.  The main interpreter will
           always have an ID of 0.  Overflow results in a RuntimeError.
           If that becomes a problem later then we can adjust, e.g. by
           using a Python int. */
        int64_t next_id;
    } interpreters;

    unsigned long main_thread;
} _PyRuntimeState3_12;


typedef union {
  _PyRuntimeState3_8  v3_8;
  _PyRuntimeState3_11 v3_11;
  _PyRuntimeState3_12 v3_12;
} _PyRuntimeState;

#endif
