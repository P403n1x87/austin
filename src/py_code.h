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

#ifndef PY_CODE_H
#define PY_CODE_H

#include "mem.h"


typedef struct {
  char          * filename;
  char          * scope;
  int             lineno;
} py_code_t;


/**
 * Create a new py_code_t object from the given remote address
 * @param  raddr_t the remote address
 * @param  int     the last instruction index from the linking frame object.
 * @return         a pointer to a new instance of py_code_t
 */
py_code_t *
py_code_new_from_raddr(raddr_t *, int);


void
py_code__destroy(py_code_t *);

#endif // PY_CODE_H
