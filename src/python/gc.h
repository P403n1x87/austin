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

#include "object.h"

// ---- include/objimpl.h -----------------------------------------------------

typedef struct {
    uintptr_t _gc_next;
    uintptr_t _gc_prev;
} PyGC_Head3_8;

// ---- internal/mem.h --------------------------------------------------------

#define NUM_GENERATIONS 3

struct gc_generation3_8 {
    PyGC_Head3_8 head;
    int          threshold; /* collection threshold */
    int          count;     /* count of allocations or collections of younger
                               generations */
};

/* Running stats per generation */
struct gc_generation_stats {
    Py_ssize_t collections;
    Py_ssize_t collected;
    Py_ssize_t uncollectable;
};

struct _gc_runtime_state3_8 {
    PyObject*                  trash_delete_later;
    int                        trash_delete_nesting;
    int                        enabled;
    int                        debug;
    struct gc_generation3_8    generations[NUM_GENERATIONS];
    PyGC_Head3_8*              generation0;
    struct gc_generation3_8    permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    int                        collecting;
};

struct _gc_runtime_state3_12 {
    /* List of objects that still need to be cleaned up, singly linked
     * via their gc headers' gc_prev pointers.  */
    PyObject* trash_delete_later;
    /* Current call-stack depth of tp_dealloc calls. */
    int       trash_delete_nesting;

    /* Is automatic collection enabled? */
    int                        enabled;
    int                        debug;
    /* linked lists of container objects */
    struct gc_generation3_8    generations[NUM_GENERATIONS];
    void*                      generation0;
    /* a permanent generation which won't be collected */
    struct gc_generation3_8    permanent_generation;
    struct gc_generation_stats generation_stats[NUM_GENERATIONS];
    /* true if we are currently running the collector */
    int                        collecting;
    /* list of uncollectable objects */
    PyObject*                  garbage;
    /* a list of callbacks to be invoked when collection is performed */
    PyObject*                  callbacks;
    /* This is the number of objects that survived the last full
       collection. It approximates the number of long lived objects
       tracked by the GC.

       (by "full collection", we mean a collection of the oldest
       generation). */
    Py_ssize_t                 long_lived_total;
    /* This is the number of objects that survived all "non-full"
       collections, and are awaiting to undergo a full collection for
       the first time. */
    Py_ssize_t                 long_lived_pending;
};

typedef union {
    struct _gc_runtime_state3_8  v3_8;
    struct _gc_runtime_state3_12 v3_12;
} GCRuntimeState;
