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

#include "error.h"
#include "logging.h"
#include "py_frame.h"
#include "version.h"


// ---- PRIVATE ---------------------------------------------------------------

#define _code__get_filename(self, pid)    _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_filename)))
#define _code__get_name(self, pid)        _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_name)))

#define _code__get_lnotab(self, pid, buf) _get_bytes_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_lnotab)), buf)

#define p_ascii_data(raddr) (raddr + sizeof(PyASCIIObject))


// ----------------------------------------------------------------------------

static inline char *
_get_string_from_raddr(pid_t pid, void * raddr) {
  PyStringObject    string;
  PyUnicodeObject3  unicode;
  char            * buffer = NULL;

  // This switch statement is required by the changes regarding the string type
  // introduced in Python 3.
  switch (py_v->py_unicode.version) {
  case 2:
    if (copy_datatype(pid, raddr, string) != sizeof(string)) {
      error = ECODEUNICODE;
    }

    else {
      ssize_t len = string.ob_base.ob_size;
      buffer = (char *) malloc(len * sizeof(char) + 1);
      if (buffer == NULL)
        error = ECODEUNICODE;
      else if (copy_memory(pid, raddr + offsetof(PyStringObject, ob_sval), len, buffer) != len) {
        error = ECODEUNICODE;
        free(buffer);
        buffer = NULL;
      }
      else {
        buffer[len] = 0;
      }
    }
    break;

  case 3:
    if (copy_datatype(pid, raddr, unicode) != sizeof(unicode)) {
      error = ECODEUNICODE;
    }
    else if (unicode._base._base.state.kind != 1) {
      error = ECODEFMT;
    }
    else if (unicode._base._base.state.compact != 1) {
      error = ECODECMPT;
    }

    else {
      ssize_t len = unicode._base._base.length;
      buffer      = (char *) malloc(len * sizeof(char) + 1);
      if (buffer == NULL)
        error = ECODEUNICODE;

      else if (copy_memory(pid, p_ascii_data(raddr), len, buffer) != len) {
        error = ECODEUNICODE;
        free(buffer);
        buffer = NULL;
      }
      else
        buffer[len] = 0;
    }
  }

  check_not_null(buffer);
  return buffer;
}


// ----------------------------------------------------------------------------
static inline int
_get_bytes_from_raddr(pid_t pid, void * raddr, unsigned char ** array) {
  ssize_t len = 0;

  if (py_v->py_bytes.version == 2) {
    PyStringObject string;
    if (copy_datatype(pid, raddr, string) != sizeof(string))
      error = ECODEBYTES;

    else {
      len = string.ob_base.ob_size + 1;
      *array = (unsigned char *) malloc(len * sizeof(char) + 1);
      if (*array == NULL) {
        // In Python 2.4, the ob_size field is of type int. If we cannot
        // allocate on the first try it's because we are getting a ridiculous
        // value for len. In that case, chop it down to an int and try again.
        // This approach is simpler than adding version support.
        len = (int) len;
        *array = (unsigned char *) malloc(len * sizeof(char) + 1);
      }
      if (*array == NULL)
        error = ECODEBYTES;

      else if (copy_memory(pid, raddr + offsetof(PyStringObject, ob_sval), len, *array) != len) {
        error = ECODEBYTES;
        free(*array);
        *array = NULL;
      }
      else {
        (*array)[len] = 0;
      }
    }
  }
  else {
    PyBytesObject bytes;

    if (copy_datatype(pid, raddr, bytes) != sizeof(bytes))
      error = ECODEBYTES;

    if ((len = bytes.ob_base.ob_size + 1) < 1)  // Include null-terminator
      error = ECODEBYTES;

    else {
      *array = (unsigned char *) malloc(len * sizeof(char));
      if (*array == NULL)
        error = ECODEBYTES;

      else if (copy_memory(pid, raddr + offsetof(PyBytesObject, ob_sval), len, *array) != len) {
        error = ECODEBYTES;
        free(*array);
        *array = NULL;
      }
    }
  }

  check_not_null(*array);
  return (error & ECODEBYTES) ? -1 : len - 1;  // The last char is guaranteed to be the null terminator
}


// ----------------------------------------------------------------------------
static inline void
_py_code__destroy(py_code_t * self) {
  if (self == NULL)
    return;

  if (self->filename != NULL) free(self->filename);
  if (self->scope    != NULL) free(self->scope);
}


// ----------------------------------------------------------------------------
static inline int
_py_code__fill_from_raddr(py_code_t * self, raddr_t * raddr, int lasti) {
  PyCodeObject   code;
  char         * filename = NULL;
  char         * name     = NULL;
  unsigned char* lnotab   = NULL;
  int            len;

  if (self == NULL)
    return 1;

  if (copy_from_raddr_v(raddr, code, py_v->py_code.size))
    error = ECODE;

  else if ((filename = _code__get_filename(&code, raddr->pid)) == NULL)
    error = ECODENOFNAME;

  else if ((name = _code__get_name(&code, raddr->pid)) == NULL)
    error = ECODENONAME;

  else if ((len = _code__get_lnotab(&code, raddr->pid, &lnotab)) < 0 || len % 2)
    error = ECODENOLINENO;

  else {
    int lineno = V_FIELD(unsigned int, code, py_code, o_firstlineno);
    for (
      register int i = 0, bc = 0;
      i < len;
      lineno += lnotab[i++]
    ) {
      bc += lnotab[i++];
      if (bc > lasti)
        break;
    }

    free(lnotab);

    self->filename = filename;
    self->scope    = name;
    self->lineno   = lineno;

    return 0;
  }

  _py_code__destroy(self);
  return 1;
}


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_frame_t *
py_frame_new_from_raddr(raddr_t * raddr) {
  PyFrameObject   frame;
  py_frame_t    * py_frame = NULL;

  if (copy_from_raddr_v(raddr, frame, py_v->py_frame.size)) {
    error = EFRAME;
    goto error;
  }

  py_frame = (py_frame_t *) calloc(1, sizeof(py_frame_t));
  if (py_frame == NULL) {
    error = EFRAME;
    goto error;
  }

  raddr_t py_code_raddr = { .pid = raddr->pid, .addr = V_FIELD(void *, frame, py_frame, o_code) };
  if (_py_code__fill_from_raddr(
    &(py_frame->code),
    &py_code_raddr,
    V_FIELD(int, frame, py_frame, o_lasti)
  )) {
    error = EFRAMENOCODE;
    goto error;
  }

  py_frame->raddr.pid  = raddr->pid;
  py_frame->raddr.addr = raddr->addr;

  py_frame->prev_raddr.pid  = raddr->pid;
  py_frame->prev_raddr.addr = V_FIELD(void *, frame, py_frame, o_back);

  py_frame->frame_no = 0;
  py_frame->prev     = NULL;
  py_frame->next     = NULL;

  py_frame->invalid = 0;

  return py_frame;

error:
  if (py_frame != NULL)
    py_frame__destroy(py_frame);

  log_error();

  return NULL;
}


// ----------------------------------------------------------------------------
py_frame_t *
py_frame__prev(py_frame_t * self) {
  if (self == NULL || self->prev_raddr.addr == NULL)
    return NULL;

  if (self->prev == NULL) {
    // Lazy-loading
    self->prev = py_frame_new_from_raddr(&(self->prev_raddr));
    if (self->prev == NULL) {
      self->invalid = 1;
      error = EFRAMEINV;
    }
    else {
      self->prev->frame_no = self->frame_no + 1;
      self->prev->next     = self;
    }
  }

  check_not_null(self->prev);
  return self->prev;
}


// ----------------------------------------------------------------------------
void
py_frame__destroy(py_frame_t * self) {
  if (self == NULL)
    return;

  _py_code__destroy(&(self->code));

  if (self->prev != NULL) {
    self->prev->next = NULL;
    py_frame__destroy(self->prev);
  }

  if (self->next != NULL) {
    self->next->prev = NULL;
    py_frame__destroy(self->next);
  }

  free(self);
}
