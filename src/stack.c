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

#define STACK_C

#include <stdint.h>
#include <stdlib.h>

#include "cache.h"
#include "error.h"
#include "frame.h"
#include "hints.h"
#include "platform.h"
#include "py_proc.h"
#include "py_string.h"
#include "python/misc.h"
#include "stack.h"
#include "version.h"

int
stack_allocate(size_t size) {
    if (isvalid(_stack))
        SUCCESS; // GCOV_EXCL_LINE

    _stack = (stack_dt*)calloc(1, sizeof(stack_dt));
    if (!isvalid(_stack)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate buffer for frame stack");
        FAIL;
    } // GCOV_EXCL_STOP

    _stack->size    = size;
    _stack->base    = (frame_t**)calloc(size, sizeof(frame_t*));
    _stack->py_base = (py_frame_t*)calloc(size, sizeof(py_frame_t));
#ifdef NATIVE
    _stack->native_base = (frame_t**)calloc(size, sizeof(frame_t*));
    _stack->kernel_base = (char**)calloc(size, sizeof(char*));
#endif

    SUCCESS;
}

void
stack_deallocate(void) {
    if (!isvalid(_stack)) // GCOV_EXCL_LINE
        return;           // GCOV_EXCL_LINE

    free(_stack->base);
    free(_stack->py_base);
#ifdef NATIVE
    free(_stack->native_base);
    free(_stack->kernel_base);
#endif

    free(_stack);
}
