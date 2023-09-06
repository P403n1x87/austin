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
//
// COPYRIGHT NOTICE: The content of this file is composed of different parts
//                   taken from different versions of the source code of
//                   Python. The authors of those sources hold the copyright
//                   for most of the content of this header file.

#ifndef PYTHON_CFRAME_H
#define PYTHON_CFRAME_H

#include <stdint.h>

#include "iframe.h"

typedef struct _PyCFrame3_11 {
    uint8_t use_tracing;  // 0 or 255 (or'ed into opcode, hence 8-bit type)
    /* Pointer to the currently executing frame (it can be NULL) */
    struct _PyInterpreterFrame3_11 *current_frame;
    struct _PyCFrame3_11 *previous;
} _PyCFrame3_11;


typedef struct {
    /* Pointer to the currently executing frame (it can be NULL) */
    void *current_frame;
    void *previous;
} _PyCFrame3_12;


typedef union {
    _PyCFrame3_11 v3_11;
    _PyCFrame3_12 v3_12;
} PyCFrame;

#endif
