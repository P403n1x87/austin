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

#ifndef PY_FRAME_H
#define PY_FRAME_H

#include "mem.h"


typedef struct {
  char          * filename;
  char          * scope;
  int             lineno;
} py_code_t;


typedef struct frame {
  raddr_t        raddr;
  raddr_t        prev_raddr;

  int            frame_no;
  struct frame * prev;
  struct frame * next;     // Make it a double-linked list for easier reverse navigation
  py_code_t      code;

  int            invalid;  // Set when prev_radd != null but prev == null.
} py_frame_t;


py_frame_t *
py_frame_new_from_raddr(raddr_t *);

/**
 * Navigate to the previous frame in the stack.
 *
 * @param   py_frame_t  self
 *
 * @return  the pointer to the previous py_frame_t object or NULL if self is
 *          at the bottom of the stack.
 */
py_frame_t *
py_frame__prev(py_frame_t *);


void
py_frame__destroy(py_frame_t *);


#endif // PY_FRAME_H
