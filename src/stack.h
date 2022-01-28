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

#include "hints.h"
#include "py_string.h"
#include "version.h"

typedef struct {
  char         * filename;
  char         * scope;
  unsigned int   line;
} frame_t;

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
frame_new(char * filename, char * scope, unsigned int line) {
  frame_t * frame = (frame_t *) malloc(sizeof(frame_t));
  if (!isvalid(frame)) {
    return NULL;
  }

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

static inline int
stack_has_cycle(void) {
  if (_stack->pointer < 2)
    return FALSE;

  // This sucks! :( Worst case is quadratic in the stack height, but if the
  // sampled stacks are short on average, it might still be faster than the
  // overhead introduced by looking up from a set-like data structure.
  py_frame_t top = _stack->py_base[_stack->pointer-1];
  for (ssize_t i = _stack->pointer - 2; i >= 0; i--) {
    if (top.origin == _stack->py_base[i].origin)
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
#define _code__get_filename(self, pid, py_v)                                   \
  _string_from_raddr(                                                          \
    pid, *((void **) ((void *) self + py_v->py_code.o_filename)), py_v         \
  )

#define _code__get_name(self, pid, py_v)                                       \
  _string_from_raddr(                                                          \
    pid, *((void **) ((void *) self + py_v->py_code.o_name)), py_v             \
  )

#define _code__get_lnotab(self, pid, len, py_v)                                \
  _bytes_from_raddr(                                                           \
    pid, *((void **) ((void *) self + py_v->py_code.o_lnotab)), len, py_v      \
  )


// ----------------------------------------------------------------------------
static inline frame_t *
_frame_from_code_raddr(raddr_t * raddr, int lasti, python_v * py_v) {
  PyCodeObject    code;
  unsigned char * lnotab = NULL;

  if (fail(copy_from_raddr_v(raddr, code, py_v->py_code.size))) {
    log_ie("Cannot read remote PyCodeObject");
    return NULL;
  }

  char * filename = _code__get_filename(&code, raddr->pid, py_v);
  if (!isvalid(filename)) {
    log_ie("Cannot get file name from PyCodeObject");
    return NULL;
  }

  char * scope = _code__get_name(&code, raddr->pid, py_v);
  if (!isvalid(scope)) {
    log_ie("Cannot get scope name from PyCodeObject");
    goto failed;
  }

  ssize_t len = 0;
  lnotab = _code__get_lnotab(&code, raddr->pid, &len, py_v);
  if (!isvalid(lnotab) || len % 2) {
    log_ie("Cannot get line number from PyCodeObject");
    goto failed;
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

  free(lnotab);

  frame_t * frame = frame_new(filename, scope, lineno);
  if (!isvalid(frame)) {
    log_e("Failed to create frame object");
    goto failed;
  }

  return frame;

failed:
  sfree(lnotab);
  sfree(filename);
  sfree(scope);
  
  return NULL;
}


#endif // PY_THREAD_C


// ----------------------------------------------------------------------------
static inline void
frame__destroy(frame_t * self) {
  if (!isvalid(self))
    return;

  sfree(self->filename);
  sfree(self->scope);

  free(self);
}

#endif // STACK_H
