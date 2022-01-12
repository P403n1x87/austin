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

#ifndef PYTHON_GC_H
#define PYTHON_GC_H

#include <stdint.h>

#include "object.h"

// ---- include/objimpl.h -----------------------------------------------------

typedef union _gc_head3_7 {
    struct {
        union _gc_head3_7 *gc_next;
        union _gc_head3_7 *gc_prev;
        Py_ssize_t gc_refs;
    } gc;
    long double dummy;  /* force worst-case alignment */
} PyGC_Head3_7;

typedef struct {
    uintptr_t _gc_next;
    uintptr_t _gc_prev;
} PyGC_Head3_8;

// ---- internal/mem.h --------------------------------------------------------

#define NUM_GENERATIONS 3

struct gc_generation3_7 {
    PyGC_Head3_7 head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};


struct gc_generation3_8 {
    PyGC_Head3_8 head;
    int threshold; /* collection threshold */
    int count; /* count of allocations or collections of younger
                  generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    Py_ssize_t collections;
    Py_ssize_t collected;
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state3_7 {
    PyObject *trash_delete_later;
    int trash_delete_nesting;
    int enabled;
    int debug;
    struct gc_generation3_7 generations[NUM_GENERATIONS];
    PyGC_Head3_7 *generation0;
    struct gc_generation3_7 permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    int collecting;
};

struct _gc_runtime_state3_8 {
    PyObject *trash_delete_later;
    int trash_delete_nesting;
    int enabled;
    int debug;
    struct gc_generation3_8 generations[NUM_GENERATIONS];
    PyGC_Head3_8 *generation0;
    struct gc_generation3_8 permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    int collecting;
};

typedef union {
    struct _gc_runtime_state3_7 v3_7;
    struct _gc_runtime_state3_8 v3_8;
} GCRuntimeState;

#endif