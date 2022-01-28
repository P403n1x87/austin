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

#ifndef HEAP_H
#define HEAP_H

#include <sys/types.h>

typedef struct {
    void   * lo;
    void   * hi;
    void   * newlo;
    void   * newhi;
} _mem_block_t;


typedef struct {
    void   * content;
    size_t   size;
} _heap_t;

#define NULL_MEM_BLOCK ((_mem_block_t) {(void *) -1, NULL, (void *) -1, NULL})

#endif
