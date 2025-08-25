// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "cache.h"
#include "mem.h"
#include "py_proc.h"
#include "stack.h"
#include "stats.h"

#define MAXLEN         1024
#define MAX_STACK_SIZE 2048

typedef struct thread {
    py_proc_t* proc;

    raddr_t addr;
    raddr_t next;

    uintptr_t tid;

    raddr_t top_frame;

    /* The per-thread datastack was introduced in Python 3.11 */
    stack_chunk_t* stack;

    tstate_status_t status;
} py_thread_t;

#define py_thread__init(_proc) {.proc = _proc}

/**
 * Fill the thread structure from the given remote address.
 *
 * @param py_thread_t  the structure to fill.
 * @param raddr_t      the remote address to read from.
 * @param py_proc_t    the Python process the thread belongs to.
 */
int
py_thread__read_remote(py_thread_t*, raddr_t);

/**
 * Get the next thread, if any.
 *
 * @param  py_thread_t  self.
 *
 * @return a pointer to the next py_thread_t instance.
 */
int
py_thread__next(py_thread_t*);

/**
 * Unwind the thread.
 *
 * @param  py_thread_t  self.
 */
void
py_thread__unwind(py_thread_t*);

/**
 * Allocate memory for dumping the thread data.
 *
 * @return either SUCCESS or FAIL.
 */
int
py_thread_allocate(void);

/**
 * Deallocate memory for dumping the thread data.
 */
void
py_thread_free(void);

#ifdef NATIVE
int
py_thread__set_idle(py_thread_t*);

int
py_thread__set_interrupted(py_thread_t*, bool);

int
py_thread__is_interrupted(py_thread_t* self);

int
py_thread__save_kernel_stack(py_thread_t*);
#endif

bool
py_thread__is_idle(py_thread_t*);
