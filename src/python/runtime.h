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

#include "interp.h"
#include "thread.h"

// ---- internal/pystate.h ----------------------------------------------------

typedef struct pyruntimestate3_7 {
    int initialized;
    int core_initialized;
    PyThreadState *finalizing;

    struct pyinterpreters3_7 {
        PyThread_type_lock mutex;
        PyInterpreterState *head;
        PyInterpreterState *main;
        int64_t next_id;
    } interpreters;
#define NEXITFUNCS 32
    void (*exitfuncs[NEXITFUNCS])(void);
    int nexitfuncs;

    struct _gc_runtime_state3_7 gc;
    // struct _warnings_runtime_state warnings;
    // struct _ceval_runtime_state ceval;
    // struct _gilstate_runtime_state gilstate;
} _PyRuntimeState3_7;

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
    struct _xidregistry {
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


typedef union {
  _PyRuntimeState3_7 v3_7;
  _PyRuntimeState3_8 v3_8;
} _PyRuntimeState;

#endif
