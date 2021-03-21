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

#ifndef PY_THREAD_H
#define PY_THREAD_H

#include <stdint.h>
#include <sys/types.h>

#include "mem.h"
#include "py_proc.h"
#include "stats.h"


typedef struct thread {
  raddr_t         raddr;
  raddr_t         next_raddr;

  py_proc_t     * proc;

  uintptr_t       tid;
  struct thread * next;

  size_t          stack_height;

  int             invalid;
} py_thread_t;


/**
 * Fill the thread structure from the given remote address.
 *
 * @param py_thread_t  the structure to fill.
 * @param raddr_t      the remote address to read from.
 * @param py_proc_t    the Python process the thread belongs to.
 */
int
py_thread__fill_from_raddr(py_thread_t *, raddr_t *, py_proc_t *);


/**
 * Get the next thread, if any.
 *
 * @param  py_thread_t  self.
 *
 * @return a pointer to the next py_thread_t instance.
 */
int
py_thread__next(py_thread_t *);


/**
 * Print the frame stack using the collapsed format.
 *
 * @param  py_thread_t  self.
 * @param  ctime_t      the time delta.
 * @param  ssize_t      the memory delta.
 */
void
py_thread__print_collapsed_stack(py_thread_t *, ctime_t, ssize_t);


/**
 * Allocate memory for dumping the frame stack.
 *
 * @return either SUCCESS or FAIL.
 */
int
py_thread_allocate_stack(void);


/**
 * Deallocate memory for dumping the frame stack.
 */
void
py_thread_free_stack(void);


#endif // PY_THREAD_H
