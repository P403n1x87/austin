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

#ifndef PY_STRING_H
#define PY_STRING_H

#include <stdlib.h>
#include <string.h>

#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "platform.h"
#include "python/string.h"
#include "version.h"

#define MAGIC_TINY                            7
#define MAGIC_BIG                       1000003
#define p_ascii_data(raddr, size)              (raddr + size)


// ----------------------------------------------------------------------------
static inline long
string__hash(char * string) {
  // Stolen from stringobject.c
  register unsigned char *p;
  register long x;

  p = (unsigned char *) string;
  x = *p << MAGIC_TINY;
  while (*p != 0)
    x = (MAGIC_BIG * x) ^ *(p++);
  x ^= strlen(string);
  return x == 0 ? 1 : x;
}


// ----------------------------------------------------------------------------
static inline char *
_string_from_raddr(proc_ref_t pref, void * raddr, python_v * py_v) {
  PyUnicodeObject    unicode;
  char             * buffer = NULL;
  ssize_t            len = 0;

  if (fail(copy_datatype(pref, raddr, unicode))) {
    log_ie("Cannot read remote PyUnicodeObject3");
    goto failed;
  }

  PyASCIIObject ascii = unicode.v3._base._base;

  if (ascii.state.kind != 1) {
    set_error(ECODEFMT);
    goto failed;
  }
  
  // Because changes to PyASCIIObject are rare, we handle the version manually
  // instead of using a version offset descriptor.
  ssize_t ascii_size = V_MIN(3, 12) ? sizeof(unicode.v3_12._base._base) : sizeof(unicode.v3._base._base);
  void * data = ascii.state.compact
    ? p_ascii_data(raddr, ascii_size)
    : (V_MIN(3, 12) ? unicode.v3_12._base.utf8 : unicode.v3._base.utf8);
  len = ascii.state.compact
    ? ascii.length
    : (V_MIN(3, 12) ? unicode.v3_12._base.utf8_length : unicode.v3._base.utf8_length);

  if (len < 0 || len > 4096) {
    log_e("Invalid string length");
    goto failed;
  }
  
  buffer = (char *) malloc(len + 1);
  
  if (!isvalid(data) || fail(copy_memory(pref, data, len, buffer))) {
    log_ie("Cannot read remote value of PyUnicodeObject3");
    goto failed;
  }
  buffer[len] = 0;

  return buffer;

failed:
  sfree(buffer);
  return NULL;
}


// ----------------------------------------------------------------------------
static inline unsigned char *
_bytes_from_raddr(proc_ref_t pref, void * raddr, ssize_t * size, python_v * py_v) {
  PyBytesObject   bytes;
  ssize_t         len = 0;
  unsigned char * array = NULL;

  if (fail(copy_datatype(pref, raddr, bytes))) {
    log_ie("Cannot read remote PyBytesObject");
    goto error;
  }

  if ((len = bytes.ob_base.ob_size + 1) < 1) { // Include null-terminator
    set_error(ECODEBYTES);
    log_e("PyBytesObject is too short");
    goto error;
  }

  array = (unsigned char *) malloc((len + 1) * sizeof(unsigned char *));
  if (fail(copy_memory(pref, raddr + offsetof(PyBytesObject, ob_sval), len, array))) {
    log_ie("Cannot read remote value of PyBytesObject");
    goto error;
  }

  array[len] = 0;
  *size      = len - 1;

  return array;

error:
  sfree(array);
  return NULL;
}


#endif
