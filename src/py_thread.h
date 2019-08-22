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

#include <sys/types.h>

#include "mem.h"
#include "stats.h"

#include "py_frame.h"


typedef struct thread {
  raddr_t         raddr;
  raddr_t         next_raddr;

  long            tid;
  struct thread * next;

  py_frame_t    * first_frame;
  py_frame_t    * last_frame;

  int             invalid;
} py_thread_t;


py_thread_t *
py_thread_new_from_raddr(raddr_t *);


/**
 * Retrieve the frame for the thread that sits at the bottom of the stack.
 *
 * @param  py_thread_t  self.
 *
 * @return a pointer to an instance of py_frame_t that represents the
 *         bottom-most entry in the frame stack for the thread.
 */
py_frame_t *
py_thread__first_frame(py_thread_t *);


/**
 * Get the next thread, if any.
 *
 * @param  py_thread_t  self.
 *
 * @return a pointer to the next py_thread_t instance.
 */
py_thread_t *
py_thread__next(py_thread_t *);


/**
 * Print the frame stack using the collapsed format.
 *
 * @param  py_thread_t  self.
 * @param  ctime_t      the time delta.
 * @param  ssize_t      the memory delta.
 *
 * @return 0 if the frame stack was printed, 1 otherwise.
 */
int
py_thread__print_collapsed_stack(py_thread_t *, ctime_t, ssize_t);


void
py_thread__destroy(py_thread_t *);


#endif // PY_THREAD_H
