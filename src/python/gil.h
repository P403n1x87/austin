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

#ifndef PYTHON_GIL_H
#define PYTHON_GIL_H

#include "interp.h"
#include "misc.h"

struct _gilstate_runtime_state3_11 {
  int check_enabled;
  _Py_atomic_address tstate_current;
  PyInterpreterState *autoInterpreterState;
  Py_tss_t autoTSSkey;
};

struct _gil_runtime_state3_11 {
  unsigned long interval;
  _Py_atomic_address last_holder;
  _Py_atomic_int locked;
};

typedef struct _gil_runtime_state3_11 gil_state_t;

#endif
