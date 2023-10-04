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

#ifndef PYTHON_IFRAME_H
#define PYTHON_IFRAME_H

#include <stdint.h>

#include "code.h"
#include "frame.h"
#include "object.h"

typedef struct _PyInterpreterFrame3_11 {
    /* "Specials" section */
    PyFunctionObject *f_func; /* Strong reference */
    PyObject *f_globals; /* Borrowed reference */
    PyObject *f_builtins; /* Borrowed reference */
    PyObject *f_locals; /* Strong reference, may be NULL */
    PyCodeObject *f_code; /* Strong reference */
    PyFrameObject *frame_obj; /* Strong reference, may be NULL */
    /* Linkage section */
    struct _PyInterpreterFrame3_11 *previous;
    // NOTE: This is not necessarily the last instruction started in the given
    // frame. Rather, it is the code unit *prior to* the *next* instruction. For
    // example, it may be an inline CACHE entry, an instruction we just jumped
    // over, or (in the case of a newly-created frame) a totally invalid value:
    _Py_CODEUNIT *prev_instr;
    int stacktop;     /* Offset of TOS from localsplus  */
    bool is_entry;  // Whether this is the "root" frame for the current _PyCFrame.
    char owner;
    /* Locals and stack */
    PyObject *localsplus[1];
} _PyInterpreterFrame3_11;


#define FRAME_OWNED_BY_CSTACK 3

typedef struct _PyInterpreterFrame3_12 {
    PyCodeObject *f_code; /* Strong reference */
    struct _PyInterpreterFrame3_12 *previous;
    PyObject *f_funcobj; /* Strong reference. Only valid if not on C stack */
    PyObject *f_globals; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_builtins; /* Borrowed reference. Only valid if not on C stack */
    PyObject *f_locals; /* Strong reference, may be NULL. Only valid if not on C stack */
    PyFrameObject *frame_obj; /* Strong reference, may be NULL. Only valid if not on C stack */
    // NOTE: This is not necessarily the last instruction started in the given
    // frame. Rather, it is the code unit *prior to* the *next* instruction. For
    // example, it may be an inline CACHE entry, an instruction we just jumped
    // over, or (in the case of a newly-created frame) a totally invalid value:
    _Py_CODEUNIT *prev_instr;
    int stacktop;  /* Offset of TOS from localsplus  */
    /* The return_offset determines where a `RETURN` should go in the caller,
     * relative to `prev_instr`.
     * It is only meaningful to the callee,
     * so it needs to be set in any CALL (to a Python function)
     * or SEND (to a coroutine or generator).
     * If there is no callee, then it is meaningless. */
    uint16_t return_offset;
    char owner;
    /* Locals and stack */
    PyObject *localsplus[1];
} _PyInterpreterFrame3_12;


typedef union {
    _PyInterpreterFrame3_11 v3_11;
    _PyInterpreterFrame3_12 v3_12;
} PyInterpreterFrame;

#endif
