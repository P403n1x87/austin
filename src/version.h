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

// This unit provides version support for Python. It summarises the ABI
// changes into structures, and the correct one is chosen at runtime and
// exposed via the global variable py_v. This variable should be used, when
// different versions of the same structures are available, to de-reference
// structure fields at the correct location.

#ifndef VERSION_H
#define VERSION_H


#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

#include "python.h"


#define VERSION                         ((major << 16) | (minor << 8))


/**
 * Get the value of a field of a versioned structure.
 *
 * It works by retrieving the field offset from the offset table set at
 * runtime, depending on the detected version of Python.
 *
 * @param  ctype   the C type of the field to retrieve, e.g. void *.
 * @param  py_obj  the address of the beginning of the actual Python structure.
 * @param  py_type the Austin representation of the Python structure, e.g. py_thread.
 * @param  field   the field of py_type to retrieve
 *
 * @return         the value of of the field of py_obj at the offset specified
 *                 by the field argument.
 */
#define V_FIELD(ctype, py_obj, py_type, field) (*((ctype*) (((void *) &py_obj) + py_v->py_type.field)))


typedef unsigned long offset_t;


typedef struct {
  ssize_t  size;

  offset_t o_filename;
  offset_t o_name;
  offset_t o_lnotab;
  offset_t o_firstlineno;
} py_code_v;


typedef struct {
  ssize_t  size;

  offset_t o_back;
  offset_t o_code;
  offset_t o_lasti;
} py_frame_v;


typedef struct {
  ssize_t  size;

  offset_t o_prev;
  offset_t o_next;
  offset_t o_interp;
  offset_t o_frame;
  offset_t o_thread_id;
} py_thread_v;


typedef struct {
  int version;
} py_unicode_v;


typedef struct {
  int version;
} py_bytes_v;


typedef struct {
  ssize_t  size;

  offset_t o_interp_head;
} py_runtime_v;


typedef struct {
  py_code_v    py_code;
  py_frame_v   py_frame;
  py_thread_v  py_thread;
  py_unicode_v py_unicode;
  py_bytes_v   py_bytes;
  py_runtime_v py_runtime;
} python_v;


void
set_version(int);


#ifndef VERSION_C
extern python_v * py_v;
#else
python_v * py_v;
#endif


#endif
