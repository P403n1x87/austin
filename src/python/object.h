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

// ---- object.h --------------------------------------------------------------

#define PyObject_HEAD     PyObject ob_base;
#define PyObject_VAR_HEAD PyVarObject ob_base;

#ifdef Py_TRACE_REFS
#define _PyObject_HEAD_EXTRA  \
    struct _object* _ob_next; \
    struct _object* _ob_prev;

#define _PyObject_EXTRA_INIT 0, 0,

#else
#define _PyObject_HEAD_EXTRA
#define _PyObject_EXTRA_INIT
#endif

typedef ssize_t Py_ssize_t;

typedef struct _object {
    _PyObject_HEAD_EXTRA ssize_t ob_refcnt;
    struct _typeobject*          ob_type;
} PyObject;

typedef struct {
    PyObject   ob_base;
    Py_ssize_t ob_size; /* Number of items in variable part */
} PyVarObject;
