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

#include <string.h>

#include "argparse.h"
#include "error.h"
#include "hints.h"
#include "logging.h"
#include "platform.h"
#include "version.h"

#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

#define MAX_STACK_SIZE              4096
#define MAXLEN                      1024


typedef struct {
  char         filename [MAXLEN];
  char         scope    [MAXLEN];
  unsigned int lineno;
} py_code_t;


typedef struct frame {
  raddr_t        raddr;
  raddr_t        prev_raddr;

  py_code_t      code;

  int            invalid;  // Set when prev_radd != null but unable to copy.
} py_frame_t;


py_frame_t * _stack = NULL;


// ---- PyCode ----------------------------------------------------------------

#define _code__get_filename(self, pid, dest)    _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_filename)), dest)
#define _code__get_name(self, pid, dest)        _get_string_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_name)), dest)

#define _code__get_lnotab(self, pid, buf)       _get_bytes_from_raddr(pid, *((void **) ((void *) self + py_v->py_code.o_lnotab)), buf)

#define p_ascii_data(raddr)                     (raddr + sizeof(PyASCIIObject))


// ----------------------------------------------------------------------------

static inline int
_get_string_from_raddr(pid_t pid, void * raddr, char * buffer) {
  PyStringObject    string;
  PyUnicodeObject3  unicode;

  // This switch statement is required by the changes regarding the string type
  // introduced in Python 3.
  switch (py_v->py_unicode.version) {
  case 2:
    if (fail(copy_datatype(pid, raddr, string))) {
      log_ie("Cannot read remote PyStringObject");
      FAIL;
    }

    ssize_t len = string.ob_base.ob_size;
    if (len >= MAXLEN)
      len = MAXLEN-1;
    if (fail(copy_memory(pid, raddr + offsetof(PyStringObject, ob_sval), len, buffer))) {
      log_ie("Cannot read remote value of PyStringObject");
      FAIL;
    }
    buffer[len] = 0;
    break;

  case 3:
    if (fail(copy_datatype(pid, raddr, unicode))) {
      log_ie("Cannot read remote PyUnicodeObject3");
      FAIL;
    }
    if (unicode._base._base.state.kind != 1) {
      set_error(ECODEFMT);
      FAIL;
    }
    if (unicode._base._base.state.compact != 1) {
      set_error(ECODECMPT);
      FAIL;
    }

    len = unicode._base._base.length;
    if (len >= MAXLEN)
      len = MAXLEN-1;

    if (fail(copy_memory(pid, p_ascii_data(raddr), len, buffer))) {
      log_ie("Cannot read remote value of PyUnicodeObject3");
      FAIL;
    }
    buffer[len] = 0;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_get_bytes_from_raddr(pid_t pid, void * raddr, unsigned char * array) {
  PyStringObject string;
  PyBytesObject  bytes;
  ssize_t        len = 0;

  if (!isvalid(array))
    goto error;

  switch (py_v->py_bytes.version) {
  case 2:  // Python 2
    if (fail(copy_datatype(pid, raddr, string))) {
      log_ie("Cannot read remote PyStringObject");
      goto error;
    }

    len = string.ob_base.ob_size + 1;
    if (len >= MAXLEN) {
      // In Python 2.4, the ob_size field is of type int. If we cannot
      // allocate on the first try it's because we are getting a ridiculous
      // value for len. In that case, chop it down to an int and try again.
      // This approach is simpler than adding version support.
      len = (int) len;
      if (len >= MAXLEN) {
        log_w("Using MAXLEN when retrieving Bytes object.");
        len = MAXLEN-1;
      }
    }

    if (fail(copy_memory(pid, raddr + offsetof(PyStringObject, ob_sval), len, array))) {
      log_ie("Cannot read remote value of PyStringObject");
      len = 0;
      goto error;
    }
    break;

  case 3:  // Python 3
    if (fail(copy_datatype(pid, raddr, bytes))) {
      log_ie("Cannot read remote PyBytesObject");
      goto error;
    }

    if ((len = bytes.ob_base.ob_size + 1) < 1) { // Include null-terminator
      set_error(ECODEBYTES);
      goto error;
    }

    if (len >= MAXLEN) {
      log_w("Using MAXLEN when retrieving Bytes object.");
      len = MAXLEN-1;
    }

    if (fail(copy_memory(pid, raddr + offsetof(PyBytesObject, ob_sval), len, array))) {
      log_ie("Cannot read remote value of PyBytesObject");
      len = 0;
      goto error;
    }
  }

  array[len] = 0;

error:
  return len - 1;  // The last char is guaranteed to be the null terminator
}


// ----------------------------------------------------------------------------
static inline int
_py_code__fill_from_raddr(py_code_t * self, raddr_t * raddr, int lasti) {
  PyCodeObject  code;
  unsigned char lnotab[MAXLEN];
  int           len;

  if (self == NULL)
    FAIL;

  if (fail(copy_from_raddr_v(raddr, code, py_v->py_code.size))) {
    log_ie("Cannot read remote PyCodeObject");
    FAIL;
  }

  if (fail(_code__get_filename(&code, raddr->pid, self->filename))) {
    log_ie("Cannot get file name from PyCodeObject");
    FAIL;
  }

  if (fail(_code__get_name(&code, raddr->pid, self->scope))) {
    log_ie("Cannot get scope name from PyCodeObject");
    FAIL;
  }

  else if ((len = _code__get_lnotab(&code, raddr->pid, lnotab)) < 0 || len % 2) {
    log_ie("Cannot get line number from PyCodeObject");
    FAIL;
  }

  int lineno = V_FIELD(unsigned int, code, py_code, o_firstlineno);
  for (register int i = 0, bc = 0; i < len; i++) {
    bc += lnotab[i++];
    if (bc > lasti)
      break;

    if (lnotab[i] >= 0x80)
      lineno -= 0x100;

    lineno += lnotab[i];
  }

  self->lineno = lineno;

  SUCCESS;
}


// ---- PyFrame ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static inline int
_py_frame__fill_from_raddr(py_frame_t * self, raddr_t * raddr) {
  PyFrameObject frame;

  self->invalid = 1;

  if (fail(copy_from_raddr_v(raddr, frame, py_v->py_frame.size))) {
    log_ie("Cannot read remote PyFrameObject");
    FAIL;
  }

  raddr_t py_code_raddr = {
    .pid  = raddr->pid,
    .addr = V_FIELD(void *, frame, py_frame, o_code)
  };
  if (_py_code__fill_from_raddr(
    &(self->code), &py_code_raddr, V_FIELD(int, frame, py_frame, o_lasti)
  )) {
    log_ie("Cannot get PyCodeObject for frame");
    SUCCESS;
  }

  self->raddr.pid  = raddr->pid;
  self->raddr.addr = raddr->addr;

  self->prev_raddr.pid  = raddr->pid;
  self->prev_raddr.addr = V_FIELD(void *, frame, py_frame, o_back);

  self->invalid = 0;

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline int
_py_frame__prev(py_frame_t * self) {
  if (!isvalid(self) || !isvalid(self->prev_raddr.addr))
    FAIL;

  raddr_t prev_raddr = {
    .pid  = self->prev_raddr.pid,
    .addr = self->prev_raddr.addr
  };

  return _py_frame__fill_from_raddr(self + 1, &prev_raddr);
}


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
py_thread__fill_from_raddr(py_thread_t * self, raddr_t * raddr) {
  PyThreadState ts;

  self->invalid      = 1;
  self->stack_height = 0;

  if (fail(copy_from_raddr(raddr, ts))) {
    log_ie("Cannot read remote PyThreadState");
    FAIL;
  }

  if (V_FIELD(void*, ts, py_thread, o_frame) != NULL) {
    raddr_t frame_raddr = { .pid = raddr->pid, .addr = V_FIELD(void*, ts, py_thread, o_frame) };
    if (fail(_py_frame__fill_from_raddr(_stack, &frame_raddr))) {
      log_d("Failed to fill last frame");
      SUCCESS;
    }
    self->stack_height = 1;

    register size_t i = 0;
    while (success(_py_frame__prev(_stack + i)) && i < MAX_STACK_SIZE) {
      if (_stack[++i].invalid) {
        log_d("Frame number %d is invalid", i);
        SUCCESS;
      }
    }
    if (i >= MAX_STACK_SIZE)
      log_w("Frames limit reached. Discarding the rest");
    self->stack_height += i ;
  }

  self->raddr.pid  = raddr->pid;
  self->raddr.addr = raddr->addr;

  self->next_raddr.pid  = raddr->pid;
  self->next_raddr.addr = V_FIELD(void*, ts, py_thread, o_next) == raddr->addr \
    ? NULL \
    : V_FIELD(void*, ts, py_thread, o_next);

  self->tid  = V_FIELD(long, ts, py_thread, o_thread_id);
  if (self->tid == 0)
    self->tid = (uintptr_t) raddr->addr;

  self->invalid = 0;
  SUCCESS;
}


// ----------------------------------------------------------------------------
int
py_thread__next(py_thread_t * self) {
  if (!isvalid(self->next_raddr.addr))
    FAIL;

  raddr_t next_raddr = { .pid = self->next_raddr.pid, .addr = self->next_raddr.addr };

  return py_thread__fill_from_raddr(self, &next_raddr);
}


// ----------------------------------------------------------------------------
#if defined PL_WIN
  #define SAMPLE_HEAD "P%I64d;T%I64x"
  #define MEM_METRIC " %I64d"
#else
  #define SAMPLE_HEAD "P%d;T%lx"
  #define MEM_METRIC " %ld"
#endif

void
py_thread__print_collapsed_stack(py_thread_t * self, ctime_t delta, ssize_t mem_delta) {
  if (!pargs.full && pargs.memory && mem_delta <= 0)
    return;

  if (self->invalid)
    return;

  if (self->stack_height == 0 && pargs.exclude_empty)
    // Skip if thread has no frames and we want to exclude empty threads
    return;

  // Group entries by thread.
  fprintf(pargs.output_file, SAMPLE_HEAD, self->raddr.pid, self->tid);

  if (self->stack_height) {
    // Append frames
    register int i = self->stack_height;
    while(i > 0) {
      py_code_t code = _stack[--i].code;
      if (pargs.sleepless && strstr(code.scope, "wait") != NULL) {
        delta = 0;
        fprintf(pargs.output_file, ";<idle>");
        break;
      }
      fprintf(pargs.output_file, pargs.format, code.scope, code.filename, code.lineno);
    }
  }
  // Finish off sample with the metric(s)
  if (pargs.full) {
    fprintf(pargs.output_file, " %lu" MEM_METRIC MEM_METRIC "\n",
      delta, mem_delta >= 0 ? mem_delta : 0, mem_delta < 0 ? mem_delta : 0
    );
  }
  else {
    if (pargs.memory)
      fprintf(pargs.output_file, MEM_METRIC "\n", mem_delta);
    else
      fprintf(pargs.output_file, " %lu\n", delta);
  }
}


// ----------------------------------------------------------------------------
int
py_thread_allocate_stack(void) {
  if (isvalid(_stack))
    SUCCESS;

  _stack = (py_frame_t *) calloc(MAX_STACK_SIZE, sizeof(py_frame_t));
  return _stack == NULL;
}


// ----------------------------------------------------------------------------
void
py_thread_free_stack(void) {
  sfree(_stack);
}
