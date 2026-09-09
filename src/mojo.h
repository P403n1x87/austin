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
#include <string.h>

#include "argparse.h"
#include "cache.h"
#include "platform.h"

#define MOJO_VERSION 4

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
    MOJO_STACK_REPEAT,
    MOJO_TASK_STACK,
    MOJO_TASK_WAITER,
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

// Output buffer: accumulate a full sample before flushing to avoid many small
// fwrite/fputc calls. 4 KiB covers the worst-case sample (hundreds of frames).
#define MOJO_BUF_SIZE 4096
static char   _mojo_buf[MOJO_BUF_SIZE];
static size_t _mojo_buf_len = 0;

static inline void
_mojo_flush(void) {
    if (_mojo_buf_len > 0) {
        fwrite(_mojo_buf, _mojo_buf_len, 1, pargs.output_file);
        _mojo_buf_len = 0;
    }
}

static inline void
_mojo_write(const void* data, size_t len) {
    if (len > MOJO_BUF_SIZE - _mojo_buf_len) {
        _mojo_flush();
        if (len >= MOJO_BUF_SIZE) {
            fwrite(data, len, 1, pargs.output_file);
            return;
        }
    }
    memcpy(_mojo_buf + _mojo_buf_len, data, len);
    _mojo_buf_len += len;
}

// Primitives

#define mojo_event(event)        \
    do {                         \
        char _e = (char)(event); \
        _mojo_write(&_e, 1);     \
    } while (0)

#define mojo_string(string)              \
    do {                                 \
        const char* _s = (string);       \
        _mojo_write(_s, strlen(_s) + 1); \
    } while (0)

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

    _mojo_write(buffer, ptr - buffer);
}

// We expect the retained bits to be varied enough to provide a valid key. This
// way we can keep the size of references to a maximum of 4 bytes.
#define mojo_ref(intorptr) (mojo_integer(MOJO_INT32 & ((mojo_int_t)(uintptr_t)intorptr), 0))

// Mojo events

#define mojo_header()                    \
    {                                    \
        fputs("MOJ", pargs.output_file); \
        mojo_integer(MOJO_VERSION, 0);   \
        _mojo_flush();                   \
        fflush(pargs.output_file);       \
    }

#define mojo_frame_ref(frame)    \
    mojo_event(MOJO_FRAME_REF);  \
    mojo_integer(frame->key, 0);

#define mojo_stack_repeat() mojo_event(MOJO_STACK_REPEAT);

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

// Introduces the coroutine stack of a suspended task, keyed by the remote
// address of its TaskObj and its (possibly cached) name (0 if unresolved).
// Its owning thread is implicit in wire position, bracketed between the
// MOJO_STACK it followed and the next one.
#define mojo_task_stack(task_id, name_key) \
    mojo_event(MOJO_TASK_STACK);           \
    mojo_ref(task_id);                     \
    mojo_ref(name_key);

// One edge of a task's waiter DAG: task_id is being awaited by waiter_id.
// Emitted once per direct waiter, for every live task that has at least one
// waiter, whenever the task's waiter-set fingerprint has changed since the
// last scan.
#define mojo_task_waiter(task_id, waiter_id) \
    mojo_event(MOJO_TASK_WAITER);            \
    mojo_ref(task_id);                       \
    mojo_ref(waiter_id);

// Emit the metric tail shared by MOJO_STACK and MOJO_STACK_REPEAT.
// Assumes pargs is in scope (always true inside the sampler/event handlers).
#define mojo_emit_metrics(sample)                        \
    {                                                    \
        if ((sample)->gc_state == GC_STATE_COLLECTING) { \
            mojo_event(MOJO_GC);                         \
        }                                                \
        if (pargs.full) {                                \
            mojo_metric_time((sample)->time);            \
            if ((sample)->is_idle) {                     \
                mojo_event(MOJO_IDLE);                   \
            }                                            \
            mojo_metric_memory((sample)->memory);        \
        } else {                                         \
            if (pargs.memory) {                          \
                mojo_metric_memory((sample)->memory);    \
            } else {                                     \
                mojo_metric_time((sample)->time);        \
            }                                            \
        }                                                \
    }
