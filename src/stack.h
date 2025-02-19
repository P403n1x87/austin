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

#pragma once

#include <stdint.h>
#include <stdlib.h>

#include "cache.h"
#include "frame.h"
#include "hints.h"
#include "mojo.h"
#include "platform.h"
#include "py_proc.h"
#include "py_string.h"
#include "python/misc.h"
#include "version.h"

typedef struct {
    size_t      size;
    frame_t**   base;
    ssize_t     pointer;
    py_frame_t* py_base;
#ifdef NATIVE
    frame_t** native_base;
    ssize_t   native_pointer;

    char**  kernel_base;
    ssize_t kernel_pointer;
#endif
} stack_dt;

static stack_dt* _stack;

static inline int
stack_allocate(size_t size) {
    if (isvalid(_stack))
        SUCCESS;

    _stack = (stack_dt*)calloc(1, sizeof(stack_dt));
    if (!isvalid(_stack))
        FAIL;

    _stack->size    = size;
    _stack->base    = (frame_t**)calloc(size, sizeof(frame_t*));
    _stack->py_base = (py_frame_t*)calloc(size, sizeof(py_frame_t));
#ifdef NATIVE
    _stack->native_base = (frame_t**)calloc(size, sizeof(frame_t*));
    _stack->kernel_base = (char**)calloc(size, sizeof(char*));
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
    py_frame_t top = _stack->py_base[_stack->pointer - 1];
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
stack_py_push(void* origin, void* code, int lasti) {
    _stack->py_base[_stack->pointer++] = (py_frame_t){.origin = origin, .code = code, .lasti = lasti};
}

#define stack_pointer() (_stack->pointer)
#define stack_push(frame)                        \
    { _stack->base[_stack->pointer++] = frame; }
#define stack_set(i, frame)      \
    { _stack->base[i] = frame; }
#define stack_pop()     (_stack->base[--_stack->pointer])
#define stack_py_pop()  (_stack->py_base[--_stack->pointer])
#define stack_py_get(i) (_stack->py_base[i])
#define stack_top()     (_stack->pointer ? _stack->base[_stack->pointer - 1] : NULL)
#define stack_reset()        \
    { _stack->pointer = 0; }
#define stack_is_valid() (_stack->base[_stack->pointer - 1]->line != 0)
#define stack_is_empty() (_stack->pointer == 0)
#define stack_full()     (_stack->pointer >= _stack->size)

#ifdef NATIVE
#define stack_py_push_cframe() (stack_py_push(CFRAME_MAGIC, NULL, 0))

#define stack_native_push(frame)                               \
    { _stack->native_base[_stack->native_pointer++] = frame; }
#define stack_native_pop()      (_stack->native_base[--_stack->native_pointer])
#define stack_native_is_empty() (_stack->native_pointer == 0)
#define stack_native_full()     (_stack->native_pointer >= _stack->size)
#define stack_native_reset()        \
    { _stack->native_pointer = 0; }

#define stack_kernel_push(frame)                               \
    { _stack->kernel_base[_stack->kernel_pointer++] = frame; }
#define stack_kernel_pop()      (_stack->kernel_base[--_stack->kernel_pointer])
#define stack_kernel_is_empty() (_stack->kernel_pointer == 0)
#define stack_kernel_full()     (_stack->kernel_pointer >= _stack->size)
#define stack_kernel_reset()        \
    { _stack->kernel_pointer = 0; }
#endif

// ----------------------------------------------------------------------------

// Support for datastack_chunk. This thread data was introduced in CPython 3.11
// and is used to store per-thread interpreter frame objects. Support for these
// chunks of memory allows us to copy all the frame objects in one go, thus
// reducing the number of syscalls needed to copy the individual frame objects.
// We expect that an added benefit of this is also a reduced error rate and
// higher overall accuracy.

// This is our representation of the linked list of stack chunks
typedef struct stack_chunk {
    void*               origin;
    _PyStackChunk*      data;
    struct stack_chunk* previous;
} stack_chunk_t;

// ----------------------------------------------------------------------------
static inline stack_chunk_t*
stack_chunk_new(proc_ref_t pref, void* origin) {
    _PyStackChunk original_chunk;

    if (copy_datatype(pref, origin, original_chunk)) {
        log_e("Failed to copy _PyStackChunk");
        return NULL;
    }

    stack_chunk_t* chunk = (stack_chunk_t*)calloc(1, sizeof(stack_chunk_t));
    if (!isvalid(chunk)) {
        log_e("Cannot allocate memory for stack chunk");
        return NULL;
    }

    chunk->data = (_PyStackChunk*)malloc(original_chunk.size);
    if (!isvalid(chunk->data)) {
        log_e("Cannot allocate memory for stack chunk data");
        goto fail;
    }

    if (copy_memory(pref, origin, original_chunk.size, chunk->data)) {
        log_e("Failed to copy full stack chunk data");
        goto fail;
    }

    chunk->origin = origin;

    if (original_chunk.previous != NULL) {
        chunk->previous = stack_chunk_new(pref, original_chunk.previous);
        if (!isvalid(chunk->previous)) {
            log_e("Failed to copy previous stack chunk");
            goto fail;
        }
    }

    return chunk;

fail:
    sfree(chunk->data);
    sfree(chunk);

    return NULL;
}

// ----------------------------------------------------------------------------
static inline void
stack_chunk__destroy(stack_chunk_t* chunk) {
    if (!isvalid(chunk))
        return;

    sfree(chunk->data);
    stack_chunk__destroy(chunk->previous);

    sfree(chunk);
}

// ----------------------------------------------------------------------------
static inline void*
stack_chunk__resolve(stack_chunk_t* self, void* address) {
    if (address >= self->origin && (char*)address < (char*)self->origin + self->data->size)
        return (char*)self->data + ((char*)address - (char*)self->origin);

    if (self->previous)
        return stack_chunk__resolve(self->previous, address);

    return NULL;
}
