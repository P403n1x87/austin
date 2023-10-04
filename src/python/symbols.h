// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#ifndef PY_SYMBOLS_H
#define PY_SYMBOLS_H

#include "../platform.h"
#include "../py_string.h"

#define DYNSYM_MANDATORY 1

enum {
  // Mandatory symbols
  DYNSYM_RUNTIME,
  // Optional symbols
  DYNSYM_HEX_VERSION,
  // Count
  DYNSYM_COUNT
};


#ifdef PY_PROC_C

#ifdef PL_MACOS
  #define SYM_PREFIX "_"
#else
  #define SYM_PREFIX ""
#endif


static const char * _dynsym_array[] = {
  SYM_PREFIX "_PyRuntime",
  SYM_PREFIX "Py_Version",
};

static long _dynsym_hash_array[DYNSYM_COUNT] = {0};

#define symcmp(name, i) ((string__hash(name) != _dynsym_hash_array[i] || strcmp(name, _dynsym_array[i])))

static inline void
_prehash_symbols(void) {
  if (_dynsym_hash_array[0] == 0) {
    for (register int i = 0; i < DYNSYM_COUNT; i++) {
      _dynsym_hash_array[i] = string__hash((char *) _dynsym_array[i]);
    }
  }
}

#endif  // PY_PROC_C


#endif  // PY_SYMBOLS_H