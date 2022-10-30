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

#pragma once


#include "py_string.h"


#define _code__get_filename(self, pref, py_v)                                  \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_filename)), py_v        \
  )

#define _code__get_name(self, pref, py_v)                                      \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_name)), py_v            \
  )

#define _code__get_qualname(self, pref, py_v)                                  \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_qualname)), py_v        \
  )

#define _code__get_lnotab(self, pref, len, py_v)                               \
  _bytes_from_raddr(                                                           \
    pref, *((void **) ((void *) self + py_v->py_code.o_lnotab)), len, py_v     \
  )
