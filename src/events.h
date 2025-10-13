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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>

#include "argparse.h"
#include "hints.h"
#include "logging.h"
#include "mojo.h"
#include "platform.h"
#include "py_string.h"

#if defined PL_WIN
#define fprintfp _fprintf_p
#else
#define fprintfp fprintf
#endif

#if defined PL_WIN
#define MEM_METRIC "%lld"
#else
#define MEM_METRIC "%zd"
#endif
#define TIME_METRIC MICROSECONDS_FMT
#define IDLE_METRIC "%d"
#define METRIC_SEP  ","

typedef enum {
    GC_STATE_INACTIVE,
    GC_STATE_COLLECTING,
    GC_STATE_UNKNOWN,
} gc_state_t;

typedef struct {
    pid_t          pid;      // Process ID
    int64_t        iid;      // Interpreter ID
    uintptr_t      tid;      // Thread ID
    microseconds_t time;     // Time of the sample
    ssize_t        memory;   // Memory usage
    gc_state_t     gc_state; // GC state
    bool           is_idle;  // Is the thread idle?
} sample_t;

struct _eh;

typedef void (*event_handler_metadata_t)(struct _eh*, char* key, char* value, va_list args);
typedef void (*event_handler_stack_begin_t)(struct _eh*, sample_t* sample);
typedef void (*event_handler_new_string_t)(struct _eh*, cached_string_t* string);
typedef void (*event_handler_new_frame_t)(struct _eh*, void* frame);
typedef void (*event_handler_stack_end_t)(struct _eh*);

typedef struct _ehs {
    event_handler_metadata_t    emit_metadata;
    event_handler_stack_begin_t emit_stack_begin;
    event_handler_new_string_t  emit_new_string;
    event_handler_new_frame_t   emit_new_frame;
    event_handler_stack_end_t   emit_stack_end;
} event_handler_spec_t;

typedef struct _eh {
    event_handler_spec_t spec; // Pointer to the event handler
    // custom event handler data
} event_handler_t;

#ifndef EVENTS_C
extern
#endif
    event_handler_t* event_handler; // Global event handler

static inline void
event_handler__emit_stack_begin(sample_t* sample) {
    if (!isvalid(event_handler)) // GCOV_EXCL_LINE
        return;                  // GCOV_EXCL_LINE

    event_handler_stack_begin_t handler = event_handler->spec.emit_stack_begin;
    if (isvalid(handler))
        handler(event_handler, sample);
}

static inline void
event_handler__emit_metadata(char* key, char* fmt, ...) {
    if (!isvalid(event_handler)) // GCOV_EXCL_LINE
        return;                  // GCOV_EXCL_LINE

    va_list args;
    va_start(args, fmt);

    event_handler_metadata_t handler = event_handler->spec.emit_metadata;
    if (isvalid(handler))
        handler(event_handler, key, fmt, args);

    va_end(args);
}

static inline void
event_handler__emit_new_string(cached_string_t* cached_string) {
    if (!isvalid(event_handler)) // GCOV_EXCL_LINE
        return;                  // GCOV_EXCL_LINE

    event_handler_new_string_t handler = event_handler->spec.emit_new_string;
    if (isvalid(handler))
        handler(event_handler, cached_string);
}

static inline void
event_handler__emit_new_frame(void* frame) {
    if (!isvalid(event_handler)) // GCOV_EXCL_LINE
        return;                  // GCOV_EXCL_LINE

    event_handler_new_frame_t handler = event_handler->spec.emit_new_frame;
    if (isvalid(handler))
        handler(event_handler, frame);
}

static inline void
event_handler__emit_stack_end(void) {
    if (!isvalid(event_handler)) // GCOV_EXCL_LINE
        return;                  // GCOV_EXCL_LINE

    event_handler_stack_end_t handler = event_handler->spec.emit_stack_end;
    if (isvalid(handler))
        handler(event_handler);
}

static inline void
event_handler_install(event_handler_t* handler) {
    if (isvalid(event_handler)) // GCOV_EXCL_LINE
        free(event_handler);    // GCOV_EXCL_LINE

    event_handler = handler;
}

static inline void
event_handler_free(void) {
    if (isvalid(event_handler)) {
        free(event_handler);
        event_handler = NULL;
    }
}

event_handler_t*
mojo_event_handler_new(void);

event_handler_t*
where_event_handler_new(void);
