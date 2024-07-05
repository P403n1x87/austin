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

// ----------------------------------------------------------------------------
// Starting with CPython 3.13, we can retrieve the offsets from a dedicated
// data structure

#define _Py_Debug_Cookie "xdebugpy"

typedef struct _Py_DebugOffsets3_13 {
    char cookie[8];  // _Py_Debug_Cookie
    uint64_t version;
    uint64_t free_threaded;
    // Runtime state offset;
    struct _runtime_state {
        uint64_t size;
        uint64_t finalizing;
        uint64_t interpreters_head;
    } runtime_state;

    // Interpreter state offset;
    struct _interpreter_state {
        uint64_t size;
        uint64_t id;
        uint64_t next;
        uint64_t threads_head;
        uint64_t gc;
        uint64_t imports_modules;
        uint64_t sysdict;
        uint64_t builtins;
        uint64_t ceval_gil;
        uint64_t gil_runtime_state;
        uint64_t gil_runtime_state_enabled;
        uint64_t gil_runtime_state_locked;
        uint64_t gil_runtime_state_holder;
    } interpreter_state;

    // Thread state offset;
    struct _thread_state{
        uint64_t size;
        uint64_t prev;
        uint64_t next;
        uint64_t interp;
        uint64_t current_frame;
        uint64_t thread_id;
        uint64_t native_thread_id;
        uint64_t datastack_chunk;
        uint64_t status;
    } thread_state;

    // InterpreterFrame offset;
    struct _interpreter_frame {
        uint64_t size;
        uint64_t previous;
        uint64_t executable;
        uint64_t instr_ptr;
        uint64_t localsplus;
        uint64_t owner;
    } interpreter_frame;

    // Code object offset;
    struct _code_object {
        uint64_t size;
        uint64_t filename;
        uint64_t name;
        uint64_t qualname;
        uint64_t linetable;
        uint64_t firstlineno;
        uint64_t argcount;
        uint64_t localsplusnames;
        uint64_t localspluskinds;
        uint64_t co_code_adaptive;
    } code_object;

    // PyObject offset;
    struct _pyobject {
        uint64_t size;
        uint64_t ob_type;
    } pyobject;

    // PyTypeObject object offset;
    struct _type_object {
        uint64_t size;
        uint64_t tp_name;
        uint64_t tp_repr;
        uint64_t tp_flags;
    } type_object;

    // PyTuple object offset;
    struct _tuple_object {
        uint64_t size;
        uint64_t ob_item;
        uint64_t ob_size;
    } tuple_object;

    // PyList object offset;
    struct _list_object {
        uint64_t size;
        uint64_t ob_item;
        uint64_t ob_size;
    } list_object;

    // PyDict object offset;
    struct _dict_object {
        uint64_t size;
        uint64_t ma_keys;
        uint64_t ma_values;
    } dict_object;

    // PyFloat object offset;
    struct _float_object {
        uint64_t size;
        uint64_t ob_fval;
    } float_object;

    // PyLong object offset;
    struct _long_object {
        uint64_t size;
        uint64_t lv_tag;
        uint64_t ob_digit;
    } long_object;

    // PyBytes object offset;
    struct _bytes_object {
        uint64_t size;
        uint64_t ob_size;
        uint64_t ob_sval;
    } bytes_object;

    // Unicode object offset;
    struct _unicode_object {
        uint64_t size;
        uint64_t state;
        uint64_t length;
        uint64_t asciiobject_size;
    } unicode_object;

    // GC runtime state offset;
    struct _gc {
        uint64_t size;
        uint64_t collecting;
    } gc;
} _Py_DebugOffsets3_13;


typedef union {
  _Py_DebugOffsets3_13 v3_13;
} _Py_DebugOffsets;


#endif
