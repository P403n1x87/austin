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

#pragma once

#include <stdio.h>

#include "argparse.h"
#include "cache.h"
#include "platform.h"

#define MOJO_VERSION 3

enum {
    MOJO_RESERVED,
    MOJO_METADATA,
    MOJO_STACK,
    MOJO_FRAME,
    MOJO_FRAME_INVALID,
    MOJO_FRAME_REF,
    MOJO_FRAME_KERNEL,
    MOJO_GC,
    MOJO_IDLE,
    MOJO_METRIC_TIME,
    MOJO_METRIC_MEMORY,
    MOJO_STRING,
    MOJO_STRING_REF,
    MOJO_MAX,
};

#if defined PL_WIN
#define FORMAT_TID "%llx"
#else
#define FORMAT_TID "%zx"
#endif

#if defined __arm__
typedef unsigned long mojo_int_t;
#else
typedef unsigned long long mojo_int_t;
#endif

// Bitmask to ensure that we encode at most 4 bytes for an integer.
#define MOJO_INT32 ((mojo_int_t)(1 << (6 + 7 * 3)) - 1)

// Primitives

#define mojo_event(event)                \
    { fputc(event, pargs.output_file); }

#define mojo_string(string) fwrite(string, strlen(string) + 1, 1, pargs.output_file);

static inline void
mojo_integer(mojo_int_t integer, int sign) {
    char  buffer[sizeof(mojo_int_t) << 1];
    char* ptr = buffer;

    unsigned char byte = integer & 0x3f;
    if (sign) {
        byte |= 0x40;
    }

    integer >>= 6;
    if (integer) {
        byte |= 0x80;
    }

    *ptr++ = byte;

    while (integer) {
        byte      = integer & 0x7f;
        integer >>= 7;
        if (integer) {
            byte |= 0x80;
        }
        *ptr++ = byte;
    }

    fwrite(buffer, ptr - buffer, 1, pargs.output_file);
}

// We expect the least significant bits to be varied enough to provide a valid
// key. This way we can keep the size of references to a maximum of 4 bytes.
#define mojo_ref(intorptr) (mojo_integer(MOJO_INT32 & ((mojo_int_t)(uintptr_t)intorptr), 0))

// Mojo events

#define mojo_header()                    \
    {                                    \
        fputs("MOJ", pargs.output_file); \
        mojo_integer(MOJO_VERSION, 0);   \
        fflush(pargs.output_file);       \
    }

#define mojo_frame_ref(frame)    \
    mojo_event(MOJO_FRAME_REF);  \
    mojo_integer(frame->key, 0);

#define mojo_frame_kernel(scope)   \
    mojo_event(MOJO_FRAME_KERNEL); \
    mojo_string(scope);

#define mojo_metric_time(value)   \
    mojo_event(MOJO_METRIC_TIME); \
    mojo_integer(value, 0);

#define mojo_metric_memory(value)                        \
    mojo_event(MOJO_METRIC_MEMORY);                      \
    mojo_integer(value < 0 ? -value : value, value < 0);

#define mojo_string_event(key, string) \
    mojo_event(MOJO_STRING);           \
    mojo_ref(key);                     \
    mojo_string(string);

#define mojo_string_ref(key)     \
    mojo_event(MOJO_STRING_REF); \
    mojo_ref(key);
