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

#define PY_THREAD_C

#include <string.h>

#include "argparse.h"
#include "error.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "platform.h"
#include "timing.h"
#include "version.h"

#include "py_thread.h"

// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of _py_thread__is_idle
// ----------------------------------------------------------------------------

#if defined(PL_LINUX)

  #include "linux/py_thread.h"

#elif defined(PL_WIN)

  #include "win/py_thread.h"

#elif defined(PL_MACOS)

  #include "mac/py_thread.h"

#endif


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
  switch (py_v->major) {
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

  switch (py_v->major) {
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

  if (py_v->major == 3 && py_v->minor >= 10) { // Python >=3.10
    lasti <<= 1;
    for (register int i = 0, bc = 0; i < len; i++) {
      int sdelta = lnotab[i++];
      if (sdelta == 0xff)
        break;

      bc += sdelta;

      int ldelta = lnotab[i];
      if (ldelta == 0x80)
        ldelta = 0;
      else if (ldelta > 0x80)
        lineno -= 0x100;

      lineno += ldelta;
      if (bc > lasti)
        break;
    }
  }
  else { // Python < 3.10
    for (register int i = 0, bc = 0; i < len; i++) {
      bc += lnotab[i++];
      if (bc > lasti)
        break;

      if (lnotab[i] >= 0x80)
        lineno -= 0x100;

      lineno += lnotab[i];
    }
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


// ----------------------------------------------------------------------------
static inline void
_py_thread__unwind_frame_stack(py_thread_t * self) {
  register size_t i = 0;
  while (success(_py_frame__prev(_stack + i)) && i < MAX_STACK_SIZE) {
    if (_stack[++i].invalid) {
      log_d("Frame number %d is invalid", i);
      return;
    }
  }
  if (i >= MAX_STACK_SIZE)
    log_w("Frames limit reached. Discarding the rest");
  self->stack_height += i;
}


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
py_thread__fill_from_raddr(py_thread_t * self, raddr_t * raddr, py_proc_t * proc) {
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
  }

  self->raddr.pid  = raddr->pid;
  self->raddr.addr = raddr->addr;

  self->proc = proc;

  self->next_raddr.pid  = raddr->pid;
  self->next_raddr.addr = V_FIELD(void*, ts, py_thread, o_next) == raddr->addr \
    ? NULL \
    : V_FIELD(void*, ts, py_thread, o_next);

  self->tid  = V_FIELD(long, ts, py_thread, o_thread_id);
  if (self->tid == 0) {
    // If we fail to get a valid Thread ID, we resort to the PyThreadState
    // remote address
    log_t("Thread ID fallback to remote address");
    self->tid = (uintptr_t) raddr->addr;
  }
  #if defined PL_LINUX
  else {
    // Try to determine the TID by reading the remote struct pthread structure.
    // We can then use this information to parse the appropriate procfs file and
    // determine the native thread's running state.
    if (unlikely(_pthread_tid_offset == 0)) {
      _infer_tid_field_offset(self);
      if (unlikely(_pthread_tid_offset == 0)) {
        log_d("tid field offset not ready");
      }
    }
    if (likely(_pthread_tid_offset != 0) && success(copy_memory(
        self->raddr.pid,
        (void *) self->tid,
        PTHREAD_BUFFER_SIZE * sizeof(void *),
        _pthread_buffer
    ))) {
      self->tid = (uintptr_t) _pthread_buffer[_pthread_tid_offset];
    }
  }
  #endif

  self->invalid = 0;
  SUCCESS;
}


// ----------------------------------------------------------------------------
int
py_thread__next(py_thread_t * self) {
  if (!isvalid(self->next_raddr.addr))
    FAIL;

  raddr_t next_raddr = { .pid = self->next_raddr.pid, .addr = self->next_raddr.addr };

  return py_thread__fill_from_raddr(self, &next_raddr, self->proc);
}


// ----------------------------------------------------------------------------
#if defined PL_WIN
  #define SAMPLE_HEAD "P%I64d;T%I64x"
  #define MEM_METRIC "%I64d"
#else
  #define SAMPLE_HEAD "P%d;T%ld"
  #define MEM_METRIC "%ld"
#endif
#define TIME_METRIC "%lu"
#define IDLE_METRIC "%d"
#define METRIC_SEP  ","

void
py_thread__print_collapsed_stack(py_thread_t * self, ctime_t time_delta, ssize_t mem_delta) {
  #if defined PL_LINUX
  // If we still don't have this offset then the thread ID is bonkers so we
  // do not emit the sample
  if (unlikely(_pthread_tid_offset == 0))
    return;
  #endif

  if (!pargs.full && pargs.memory && mem_delta == 0)
    return;

  if (self->invalid)
    return;

  if (self->stack_height == 0 && pargs.exclude_empty)
    // Skip if thread has no frames and we want to exclude empty threads
    return;

  if (mem_delta == 0 && time_delta == 0)
    return;

  int is_idle = FALSE;
  if (pargs.full || pargs.sleepless) {
    is_idle = _py_thread__is_idle(self);
    if (!pargs.full && is_idle && pargs.sleepless)
      return;
  }

  // Group entries by thread.
  fprintf(pargs.output_file, SAMPLE_HEAD, self->proc->pid, self->tid);

  if (self->stack_height) {
    _py_thread__unwind_frame_stack(self);

    // Append frames
    register int i = self->stack_height;
    while (i > 0) {
      py_code_t code = _stack[--i].code;
      fprintf(pargs.output_file, pargs.format, code.filename, code.scope, code.lineno);
    }
  }

  if (pargs.gc && py_proc__is_gc_collecting(self->proc) == TRUE) {
    fprintf(pargs.output_file, ";:GC:");
    stats_gc_time(time_delta);
  }

  // Finish off sample with the metric(s)
  if (pargs.full) {
    fprintf(pargs.output_file, " " TIME_METRIC METRIC_SEP IDLE_METRIC METRIC_SEP MEM_METRIC "\n",
      time_delta, !!is_idle, mem_delta
    );
  }
  else {
    if (pargs.memory)
      fprintf(pargs.output_file, " " MEM_METRIC "\n", mem_delta);
    else
      fprintf(pargs.output_file, " " TIME_METRIC "\n", time_delta);
  }

  // Update sampling stats
  stats_count_sample();
  if (austin_errno != EOK)
    stats_count_error();
  stats_check_duration(stopwatch_duration());
} /* py_thread__print_collapsed_stack */


// ----------------------------------------------------------------------------
int
py_thread_allocate_stack(void) {
  if (isvalid(_stack))
    SUCCESS;

  _stack = (py_frame_t *) calloc(MAX_STACK_SIZE, sizeof(py_frame_t));
  if (!isvalid(_stack))
    FAIL;

  #if defined PL_WIN
  // On Windows we need to fetch process and thread information to detect idle
  // threads. We allocate a buffer for periodically fetching that data and, if
  // needed we grow it at runtime.
  _pi_buffer_size = (1 << 16) * sizeof(void *);
  _pi_buffer      = calloc(1, _pi_buffer_size);
  if (!isvalid(_pi_buffer))
    FAIL;
  #endif

  SUCCESS;
}


// ----------------------------------------------------------------------------
void
py_thread_free_stack(void) {
  #if defined PL_WIN
  sfree(_pi_buffer);
  #endif

  sfree(_stack);
}
