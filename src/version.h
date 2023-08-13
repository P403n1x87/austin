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

#include "logging.h"
#include "platform.h"
#include "python/abi.h"


#define PYVERSION(major, minor, patch)  ((major << 16) | (minor << 8) | patch)
#define MAJOR(x)                        (x >> 16)
#define MINOR(x)                        ((x >> 8) & 0xFF)
#define PATCH(x)                        (x & 0xFF)


/**
 * Get the value of a field of a versioned structure.
 *
 * It works by retrieving the field offset from the offset table set at
 * runtime, depending on the detected version of Python. The scope in which
 * these macros are used must have a local variable py_v of type python_v *
 * declared (and initialised to a valid value). The V_DESC macro can be used to
 * ensure this.
 *
 * @param  ctype   the C type of the field to retrieve, e.g. void *.
 * @param  py_obj  the address of the beginning of the actual Python structure.
 * @param  py_type the Austin representation of the Python structure, e.g. py_thread.
 * @param  field   the field of py_type to retrieve
 *
 * @return         the value of of the field of py_obj at the offset specified
 *                 by the field argument.
 */
#define V_FIELD(ctype, py_obj, py_type, field)                                 \
  (*((ctype*) (((void *) &py_obj) + py_v->py_type.field)))

#define V_FIELD_PTR(ctype, py_obj_ptr, py_type, field)                         \
  (*((ctype*) (((void *) py_obj_ptr) + py_v->py_type.field)))

#define V_DESC(desc) python_v * py_v = (desc)

/**
 * Ensure the current version of Python is at least/at most the request version.
 * 
 * @param  M  requested major version
 * @param  m  requested minor version
 * 
 * @return    TRUE if the current version is at least/at most the requested one,
 *            FALSE otherwise.
 */
#define V_MIN(M, m) (((py_v->major << 8) | py_v->minor) >= ((M << 8) | m))
#define V_MAX(M, m) (((py_v->major << 8) | py_v->minor) <= ((M << 8) | m))

typedef unsigned long offset_t;


typedef struct {
  ssize_t  size;

  offset_t o_filename;
  offset_t o_name;
  offset_t o_lnotab;
  offset_t o_firstlineno;
  offset_t o_code;
  offset_t o_qualname;
} py_code_v;


typedef struct {
  ssize_t  size;

  offset_t o_back;
  offset_t o_code;
  offset_t o_lasti;
  offset_t o_lineno;
} py_frame_v;

typedef struct {
  ssize_t  size;

  offset_t o_current_frame;
  offset_t o_previous;
} py_cframe_v;


typedef struct {
  ssize_t  size;

  offset_t o_code;
  offset_t o_previous;
  offset_t o_prev_instr;
  offset_t o_is_entry;
} py_iframe_v;


typedef struct {
  ssize_t  size;

  offset_t o_prev;
  offset_t o_next;
  offset_t o_interp;
  offset_t o_frame;
  offset_t o_thread_id;
  offset_t o_native_thread_id;
  offset_t o_stack;
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
  offset_t o_gc;
} py_runtime_v;


typedef struct {
  ssize_t  size;

  offset_t o_next;
  offset_t o_tstate_head;
  offset_t o_gc;
} py_is_v;


typedef struct {
  ssize_t  size;

  offset_t o_collecting;
} py_gc_v;

typedef struct {
  py_code_v    py_code;
  py_frame_v   py_frame;
  py_thread_v  py_thread;
  py_is_v      py_is;
  py_runtime_v py_runtime;
  py_gc_v      py_gc;
  py_cframe_v  py_cframe;
  py_iframe_v  py_iframe;

  int          major;
  int          minor;
  int          patch;
} python_v;


#ifdef PY_PROC_C

#define UNSUPPORTED_VERSION                                                    \
  log_w("Unsupported Python version detected. Austin might not work as expected.")

#define LATEST_VERSION                  (&python_v3_11)

#define PY_CODE(s) {                    \
  sizeof(s),                            \
  offsetof(s, co_filename),             \
  offsetof(s, co_name),                 \
  offsetof(s, co_lnotab),               \
  offsetof(s, co_firstlineno)           \
}

#define PY_CODE_311(s) {                \
  sizeof(s),                            \
  offsetof(s, co_filename),             \
  offsetof(s, co_name),                 \
  offsetof(s, co_linetable),            \
  offsetof(s, co_firstlineno),          \
  offsetof(s, co_code_adaptive),        \
  offsetof(s, co_qualname),             \
}


#define PY_FRAME(s) {                   \
  sizeof(s),                            \
  offsetof(s, f_back),                  \
  offsetof(s, f_code),                  \
  offsetof(s, f_lasti),                 \
  offsetof(s, f_lineno),                \
}

#define PY_CFRAME_311(s) {              \
  sizeof(s),                            \
  offsetof(s, current_frame),           \
  offsetof(s, previous),                \
}

#define PY_IFRAME_311(s) {              \
  sizeof(s),                            \
  offsetof(s, f_code),                  \
  offsetof(s, previous),                \
  offsetof(s, prev_instr),              \
  offsetof(s, is_entry),                \
}

#define PY_THREAD(s) {                  \
  sizeof(s),                            \
  offsetof(s, prev),                    \
  offsetof(s, next),                    \
  offsetof(s, interp),                  \
  offsetof(s, frame),                   \
  offsetof(s, thread_id)                \
}

#define PY_THREAD_311(s) {              \
  sizeof(s),                            \
  offsetof(s, prev),                    \
  offsetof(s, next),                    \
  offsetof(s, interp),                  \
  offsetof(s, cframe),                  \
  offsetof(s, thread_id),               \
  offsetof(s, native_thread_id),        \
  offsetof(s, datastack_chunk),         \
}

#define PY_UNICODE(n) {                 \
  n                                     \
}

#define PY_BYTES(n) {                   \
  n                                     \
}

#define PY_RUNTIME(s) {                 \
  sizeof(s),                            \
  offsetof(s, interpreters.head),       \
  offsetof(s, gc),                      \
}

#define PY_RUNTIME_311(s) {             \
  sizeof(s),                            \
  offsetof(s, interpreters.head),       \
}


#define PY_IS(s) {                      \
  sizeof(s),                            \
  offsetof(s, next),                    \
  offsetof(s, tstate_head),             \
  offsetof(s, gc),                      \
}

#define PY_IS_311(s) {                  \
  sizeof(s),                            \
  offsetof(s, next),                    \
  offsetof(s, threads.head),            \
  offsetof(s, gc),                      \
}


#define PY_GC(s) {                      \
  sizeof(s),                            \
  offsetof(s, collecting),              \
}

// ---- Python 3.8 ------------------------------------------------------------

python_v python_v3_8 = {
  PY_CODE     (PyCodeObject3_8),
  PY_FRAME    (PyFrameObject3_7),
  PY_THREAD   (PyThreadState3_8),
  PY_IS       (PyInterpreterState2),
  PY_RUNTIME  (_PyRuntimeState3_8),
  PY_GC       (struct _gc_runtime_state3_8),
};

// ---- Python 3.9 ------------------------------------------------------------

python_v python_v3_9 = {
  PY_CODE     (PyCodeObject3_8),
  PY_FRAME    (PyFrameObject3_7),
  PY_THREAD   (PyThreadState3_8),
  PY_IS       (PyInterpreterState3_9),
  PY_RUNTIME  (_PyRuntimeState3_8),
  PY_GC       (struct _gc_runtime_state3_8),
};


// ---- Python 3.10 -----------------------------------------------------------

python_v python_v3_10 = {
  PY_CODE     (PyCodeObject3_8),
  PY_FRAME    (PyFrameObject3_10),
  PY_THREAD   (PyThreadState3_8),
  PY_IS       (PyInterpreterState3_9),
  PY_RUNTIME  (_PyRuntimeState3_8),
  PY_GC       (struct _gc_runtime_state3_8),
};

// ---- Python 3.11 -----------------------------------------------------------

python_v python_v3_11 = {
  PY_CODE_311     (PyCodeObject3_11),
  PY_FRAME        (PyFrameObject3_10),  // Irrelevant
  PY_THREAD_311   (PyThreadState3_11),
  PY_IS_311       (PyInterpreterState3_11),
  PY_RUNTIME_311  (_PyRuntimeState3_11),
  PY_GC           (struct _gc_runtime_state3_8),
  PY_CFRAME_311   (_PyCFrame3_11),
  PY_IFRAME_311   (_PyInterpreterFrame3_11),
};

// ----------------------------------------------------------------------------
static inline python_v *
get_version_descriptor(int major, int minor, int patch) {
  if (major == 0 && minor == 0)
    return NULL;

  python_v * py_v = NULL;

  switch (major) {

  // ---- Python 3 ------------------------------------------------------------
  case 3:
    switch (minor) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
      UNSUPPORTED_VERSION;  // NOTE: These versions haven't been tested.

    // 3.8
    case 8: py_v = &python_v3_8; break;
    
    //, 3.9
    case 9: py_v = &python_v3_9; break;

    // 3.10
    case 10: py_v = &python_v3_10; break;

    // 3.11
    case 11: py_v = &python_v3_11; break;

    default: py_v = LATEST_VERSION;
      UNSUPPORTED_VERSION;
    }
  }

  py_v->major = major;
  py_v->minor = minor;
  py_v->patch = patch;

  return py_v;
}

#endif  // PY_PROC_C

#endif
