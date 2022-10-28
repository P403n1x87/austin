// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2021 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#ifndef STACK_H
#define STACK_H

#include <stdint.h>
#include <stdlib.h>

#include "cache.h"
#include "hints.h"
#include "mojo.h"
#include "platform.h"
#include "py_proc.h"
#include "py_string.h"
#include "py_thread.h"
#include "version.h"

typedef struct {
  key_dt         key;
  char         * filename;
  char         * scope;
  unsigned int   line;
} frame_t;

#define py_frame_key(code, lasti)  (((key_dt) (((key_dt) code) & MOJO_INT32) << 16) | lasti)
#define py_string_key(code, field) ((key_dt) *((void **) ((void *) &code + py_v->py_code.field)))

#ifdef PY_THREAD_C

typedef struct {
  void * origin;
  void * code;
  int    lasti;
} py_frame_t;

typedef struct {
  size_t        size;
  frame_t    ** base;
  ssize_t       pointer;
  py_frame_t  * py_base;
  #ifdef NATIVE
  frame_t    ** native_base;
  ssize_t       native_pointer;

  char       ** kernel_base;
  ssize_t       kernel_pointer;
  #endif
} stack_dt;

static stack_dt * _stack;

static inline frame_t *
frame_new(key_dt key, char * filename, char * scope, unsigned int line) {
  frame_t * frame = (frame_t *) malloc(sizeof(frame_t));
  if (!isvalid(frame)) {
    return NULL;
  }

  frame->key      = key;
  frame->filename = filename;
  frame->scope    = scope;
  frame->line     = line;

  return frame;
}

static inline int
stack_allocate(size_t size) {
  if (isvalid(_stack))
    SUCCESS;

  _stack = (stack_dt *) calloc(1, sizeof(stack_dt));
  if (!isvalid(_stack))
    FAIL;
  
  _stack->size    = size;
  _stack->base    = (frame_t **)   calloc(size, sizeof(frame_t *));
  _stack->py_base = (py_frame_t *) calloc(size, sizeof(py_frame_t));
  #ifdef NATIVE
  _stack->native_base = (frame_t **) calloc(size, sizeof(frame_t *));
  _stack->kernel_base = (char **)    calloc(size, sizeof(char *));
  #endif

  SUCCESS;
}

static inline void
stack_deallocate(void) {
  if (!isvalid(_stack))
    return;

  free(_stack->base);
  free(_stack->py_base);
  #ifdef NATIVE
  free(_stack->native_base);
  free(_stack->kernel_base);
  #endif

  free(_stack);
}

#ifdef NATIVE
#define CFRAME_MAGIC             ((void*) 0xCF)
#endif

static inline int
stack_has_cycle(void) {
  if (_stack->pointer < 2)
    return FALSE;

  // This sucks! :( Worst case is quadratic in the stack height, but if the
  // sampled stacks are short on average, it might still be faster than the
  // overhead introduced by looking up from a set-like data structure.
  py_frame_t top = _stack->py_base[_stack->pointer-1];
  for (ssize_t i = _stack->pointer - 2; i >= 0; i--) {
    #ifdef NATIVE
    if (top.origin == _stack->py_base[i].origin && top.origin != CFRAME_MAGIC)
    #else
    if (top.origin == _stack->py_base[i].origin)
    #endif
      return TRUE;
  }
  return FALSE;
}

static inline void
stack_py_push(void * origin, void * code, int lasti) {
  _stack->py_base[_stack->pointer++] = (py_frame_t) {
    .origin = origin,
    .code   = code,
    .lasti  = lasti
  };
}

#define stack_pointer()         (_stack->pointer)
#define stack_push(frame)       {_stack->base[_stack->pointer++] = frame;}
#define stack_set(i, frame)     {_stack->base[i] = frame;}
#define stack_pop()             (_stack->base[--_stack->pointer])
#define stack_py_pop()          (_stack->py_base[--_stack->pointer])
#define stack_py_get(i)         (_stack->py_base[i])
#define stack_top()             (_stack->pointer ? _stack->base[_stack->pointer-1] : NULL)
#define stack_reset()           {_stack->pointer = 0;}
#define stack_is_valid()        (_stack->base[_stack->pointer-1]->line != 0)
#define stack_is_empty()        (_stack->pointer == 0)
#define stack_full()            (_stack->pointer >= _stack->size)

#ifdef NATIVE
#define stack_py_push_cframe()   (stack_py_push(CFRAME_MAGIC, NULL, 0))

#define stack_native_push(frame) {_stack->native_base[_stack->native_pointer++] = frame;}
#define stack_native_pop()       (_stack->native_base[--_stack->native_pointer])
#define stack_native_is_empty()  (_stack->native_pointer == 0)
#define stack_native_full()      (_stack->native_pointer >= _stack->size)
#define stack_native_reset()     {_stack->native_pointer = 0;}

#define stack_kernel_push(frame) {_stack->kernel_base[_stack->kernel_pointer++] = frame;}
#define stack_kernel_pop()       (_stack->kernel_base[--_stack->kernel_pointer])
#define stack_kernel_is_empty()  (_stack->kernel_pointer == 0)
#define stack_kernel_full()      (_stack->kernel_pointer >= _stack->size)
#define stack_kernel_reset()     {_stack->kernel_pointer = 0;}
#endif


// ----------------------------------------------------------------------------
#define _code__get_filename(self, pref, py_v)                                   \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_filename)), py_v         \
  )

#define _code__get_name(self, pref, py_v)                                       \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_name)), py_v             \
  )

#define _code__get_qualname(self, pref, py_v)                                   \
  _string_from_raddr(                                                          \
    pref, *((void **) ((void *) self + py_v->py_code.o_qualname)), py_v         \
  )

#define _code__get_lnotab(self, pref, len, py_v)                                \
  _bytes_from_raddr(                                                           \
    pref, *((void **) ((void *) self + py_v->py_code.o_lnotab)), len, py_v      \
  )


// ----------------------------------------------------------------------------
static inline int
_read_varint(unsigned char * lnotab, size_t * i) {
  int val = lnotab[++*i] & 63;
  int shift = 0;
  while (lnotab[*i] & 64) {
    shift += 6;
    val |= (lnotab[++*i] & 63) << shift;
  }
  return val;
}


// ----------------------------------------------------------------------------
static inline int
_read_signed_varint(unsigned char * lnotab, size_t * i) {
  int val = _read_varint(lnotab, i);
  return (val & 1) ? -(val >> 1) : (val >> 1);
}

// ----------------------------------------------------------------------------
static inline frame_t *
_frame_from_code_raddr(py_thread_t * py_thread, void * code_raddr, int lasti, python_v * py_v) {
  PyCodeObject    code;
  unsigned char * lnotab = NULL;
  py_proc_t     * py_proc = py_thread->proc;
  proc_ref_t      pref = py_thread->raddr.pref;

  if (fail(copy_py(pref, code_raddr, py_code, code))) {
    log_ie("Cannot read remote PyCodeObject");
    return NULL;
  }

  lru_cache_t * cache = py_proc->string_cache;

  key_dt string_key = py_string_key(code, o_filename);
  char * filename = lru_cache__maybe_hit(cache, string_key);
  if (!isvalid(filename)) {
    filename = _code__get_filename(&code, pref, py_v);
    if (!isvalid(filename)) {
      log_ie("Cannot get file name from PyCodeObject");
      return NULL;
    }
    lru_cache__store(cache, string_key, filename);
    if (pargs.binary) {
      mojo_string_event(string_key, filename);
    }
  }
  if (pargs.binary) {
    filename = (char *) string_key;
  }

  string_key = V_MIN(3, 11) ? py_string_key(code, o_qualname) : py_string_key(code, o_name);
  char * scope = lru_cache__maybe_hit(cache, string_key);
  if (!isvalid(scope)) {
    scope = V_MIN(3, 11)
      ? _code__get_qualname(&code, pref, py_v)
      : _code__get_name(&code, pref, py_v);
    if (!isvalid(scope)) {
      log_ie("Cannot get scope name from PyCodeObject");
      return NULL;
    }
    lru_cache__store(cache, string_key, scope);
    if (pargs.binary) {
      mojo_string_event(string_key, scope);
    }
  }
  if (pargs.binary) {
    scope = (char *) string_key;
  }

  ssize_t len = 0;
  int lineno = V_FIELD(unsigned int, code, py_code, o_firstlineno);

  if (V_MIN(3, 11)) {
    lnotab = _code__get_lnotab(&code, pref, &len, py_v);
    if (!isvalid(lnotab) || len == 0) {
      log_ie("Cannot get line information from PyCodeObject");
      goto failed;
    }

    lasti >>= 1;

    for (size_t i = 0, bc = 0; i < len; i++) {
      bc += (lnotab[i] & 7) + 1;
      int code = (lnotab[i] >> 3) & 15;
      switch (code) {
        case 15:
          break;

        case 14: // Long form
          lineno += _read_signed_varint(lnotab, &i);
          _read_varint(lnotab, &i); // end line
          _read_varint(lnotab, &i); // column
          _read_varint(lnotab, &i); // end column
          break;

        case 13: // No column data
          lineno += _read_signed_varint(lnotab, &i);
          break;

        case 12: // New lineno
        case 11:
        case 10:
          lineno += code - 10;
          i += 2; // skip column + end column
          break;

        default:
          i++; // skip column
      }
      
      if (bc > lasti)
        break;
    }
  }
  else {
    lnotab = _code__get_lnotab(&code, pref, &len, py_v);
    if (!isvalid(lnotab) || len % 2) {
      log_ie("Cannot get line information from PyCodeObject");
      goto failed;
    }

    if (V_MIN(3, 10)) {
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
  }

  free(lnotab);

  frame_t * frame = frame_new(py_frame_key(code_raddr, lasti), filename, scope, lineno);
  if (!isvalid(frame)) {
    log_e("Failed to create frame object");
    goto failed;
  }

  return frame;

failed:
  sfree(lnotab);
  
  return NULL;
}


#endif // PY_THREAD_C


// ----------------------------------------------------------------------------
static inline void
frame__destroy(frame_t * self) {
  sfree(self);
}

#endif // STACK_H
