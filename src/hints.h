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

#pragma once

#include "error.h"

#define SUCCESS return 0
#define FAIL            \
    {                   \
        log_location(); \
        return 1;       \
    }
#define STOP(x)                     \
    {                               \
        austin_errno = AUSTIN_E##x; \
        return 1;                   \
    }
#define FAIL_PTR        \
    {                   \
        log_location(); \
        return NULL;    \
    }

#define FAIL_INT        \
    {                   \
        log_location(); \
        return -1;      \
    }

#define FAIL_BOOL       \
    {                   \
        log_location(); \
        return -1;      \
    }

#define FAIL_BREAK      \
    {                   \
        log_location(); \
        break;          \
    }
#define FAIL_GOTO(x)    \
    {                   \
        log_location(); \
        goto x;         \
    }
#define FAIL_VOID       \
    {                   \
        log_location(); \
        return;         \
    }

#define success(x) (!(x))
#define fail(x)    (x)
#define sfree(x)           \
    {                      \
        if ((x) != NULL) { \
            free(x);       \
            x = NULL;      \
        }                  \
    }

#define isvalid(x) ((x) != NULL)

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
