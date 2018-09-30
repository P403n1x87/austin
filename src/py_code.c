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

#include <stddef.h>

#include "error.h"
#include "logging.h"
#include "py_code.h"
#include "version.h"


// ---- PRIVATE ---------------------------------------------------------------

#define _code__get_filename(self, pid)    _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_filename)))
#define _code__get_name(self, pid)        _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_name)))

#define _code__get_lnotab(self, pid, buf) _get_bytes_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_lnotab)), buf)

#define p_ascii_data(raddr) (raddr + sizeof(PyASCIIObject))


// ----------------------------------------------------------------------------
static char *
_get_string_from_raddr(pid_t pid, void * raddr) {
  PyUnicodeObject   unicode;
  char            * string = NULL;

  if (copy_datatype(pid, raddr, unicode) != sizeof(unicode)) {
    error = ECODE;
  }
  else if (unicode._base._base.state.kind != 1) {
    error = ECODEFMT;
  }
  else if (unicode._base._base.state.compact != 1) {
    error = ECODECMPT;
  }

  else {
    ssize_t len = unicode._base._base.length;
    string      = (char *) malloc(len * sizeof(char) + 1);
    if (string == NULL)
      error = ECODE;

    else if (copy_memory(pid, p_ascii_data(raddr), len, string) != len) {
      error = ECODE;
      free(string);
    }
    else
      string[len] = 0;
  }

  check_not_null(string);
  return string;
}


// ----------------------------------------------------------------------------
static int
_get_bytes_from_raddr(pid_t pid, void * raddr, char ** array) {
  PyBytesObject bytes;
  ssize_t       len = 0;

  if (copy_datatype(pid, raddr, bytes) != sizeof(bytes))
    error = ECODEBYTES;

  if ((len = bytes.ob_base.ob_size + 1) < 1)  // Include null-terminator
    error = ECODEBYTES;

  else {
    *array = (char *) malloc(len * sizeof(char));
    if (*array == NULL)
      error = ECODEBYTES;

    else if (copy_memory(pid, raddr + offsetof(PyBytesObject, ob_sval), len, *array) != len) {
      error = ECODEBYTES;
      free(*array);
      *array = NULL;
    }
  }

  check_not_null(*array);
  return len - 1;  // The last char is guaranteed to be the null terminator
}


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_code_t *
py_code_new_from_raddr(raddr_t * raddr, int lasti) {
  PyCodeObject   code;
  py_code_t    * py_code  = NULL;
  char         * filename = NULL;
  char         * name     = NULL;
  char         * lnotab   = NULL;
  int            len;

  if (copy_from_raddr_v(raddr, code, py_v->py_code.size))
    error = ECODE;

  else if ((filename = _code__get_filename(&code, raddr->pid)) == NULL)
    error = ECODENOFNAME;

  else if ((name = _code__get_name(&code, raddr->pid)) == NULL)
    error = ECODENONAME;

  else if ((len = _code__get_lnotab(&code, raddr->pid, &lnotab)) < 0 || len % 2)
    error = ECODENOLINENO;

  else {
    int lineno = V_FIELD(int, code, py_code, o_firstlineno);
    for (register int i = 0, bc = 0; i < len; lineno += (int) lnotab[i++]) {
      bc += (int) lnotab[i++];
      if (bc > lasti)
        break;
    }

    free(lnotab);

    // Allocate the new py_code_t object and initialise it
    py_code = (py_code_t *) malloc(sizeof(py_code_t));
    if (py_code == NULL)
      error = ECODE;

    else {
      py_code->filename = filename;
      py_code->scope    = name;
      py_code->lineno   = lineno;
    }
  }
  if (py_code == NULL) {
    if (filename != NULL) free(filename);
    if (name     != NULL) free(name);
  }

  check_not_null(py_code);
  return py_code;
}


// ----------------------------------------------------------------------------
void
py_code__destroy(py_code_t * self) {
  if (self == NULL)
    return;

  if (self->filename != NULL) free(self->filename);
  if (self->scope    != NULL) free(self->scope);

  free(self);
}
