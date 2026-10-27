// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool   logging;
    size_t page_size_cap;
} parsed_env_t;

#ifndef ENV_C
extern parsed_env_t env;
#endif

// ----------------------------------------------------------------------------
int
parse_env();

/**
 * Get the value of an environment variable of the current process.
 * @param  name  the name of the variable.
 * @return       a newly allocated copy of the value, or NULL if the variable is
 *               not set or empty.
 */
char*
env_get(const char*);

/**
 * Look up an environment variable in an environment block, such as the one of
 * another process.
 * @param  block  a block of NUL-separated NAME=VALUE entries, terminated by an
 *                empty entry or by the end of the block. The block must be
 *                NUL-terminated.
 * @param  size   the size of the block, excluding the terminating NUL.
 * @param  name   the name of the variable. Case-insensitive on Windows.
 * @return        a newly allocated copy of the value, or NULL if the variable is
 *                not set or empty.
 */
char*
env_lookup(const char*, size_t, const char*);
