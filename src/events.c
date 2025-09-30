// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#define EVENTS_C

#include "events.h"
#include "ansi.h"
#include "argparse.h"
#include "frame.h"
#include "platform.h"
#include "stack.h"

typedef struct {
    event_handler_spec_t spec;        // Pointer to the event handler
    sample_t             sample_data; // Sample data
} base_event_handler_t;

static inline void
base_event_handler__handle_stack_begin(base_event_handler_t* self, sample_t* sample) {
    self->sample_data = *sample;
}

// ----------------------------------------------------------------------------
// MOJO (binary mode) stack event handler

static inline void
mojo_event_handler__handle_stack_begin(base_event_handler_t* self, sample_t* sample) {
    char thread_name[64];

    base_event_handler__handle_stack_begin(self, sample);

    sprintf(thread_name, FORMAT_TID, sample->tid);

    mojo_event(MOJO_STACK);
    mojo_integer(sample->pid, 0);
    mojo_integer(sample->iid, 0);
    mojo_string(thread_name);
}

static inline void
mojo_event_handler__handle_metadata(base_event_handler_t* self, char* key, char* value, va_list args) {
    mojo_event(MOJO_METADATA);
    mojo_string(key);
    vfprintf(pargs.output_file, value, args);
    fputc('\0', pargs.output_file);

    // In pipe mode we do Austin event buffering.
    if (pargs.pipe)
        fflush(pargs.output_file);
}

static inline void
mojo_event_handler__handle_new_string(base_event_handler_t* self, cached_string_t* string) {
    mojo_event(MOJO_STRING);
    mojo_ref(string->key);
    mojo_string(string->value);
}

static inline void
mojo_event_handler__handle_new_frame(base_event_handler_t* self, frame_t* frame) {
    mojo_event(MOJO_FRAME);
    mojo_integer(frame->key, 0);
    mojo_ref(frame->filename->key);
    mojo_ref(frame->scope->key);
    mojo_integer(frame->line, 0);
    mojo_integer(frame->line_end, 0);
    mojo_integer(frame->column, 0);
    mojo_integer(frame->column_end, 0);
}

static inline void
mojo_event_handler__handle_stack_end(base_event_handler_t* self) {
#ifdef NATIVE
    bool has_cframes = false;
    if (stack_top() == CFRAME_MAGIC) {
        has_cframes = true;
        (void)stack_pop();
    }

    while (!stack_native_is_empty()) {
        frame_t* native_frame = stack_native_pop();
        if (!isvalid(native_frame)) {
            log_e("Invalid native frame"); // GCOV_EXCL_START
            break;                         // GCOV_EXCL_STOP
        }
        cached_string_t* scope = native_frame->scope;
        bool             is_frame_eval
            = (scope == UNKNOWN_SCOPE) ? false : isvalid(strstr(scope->value, "PyEval_EvalFrameDefault"));
        if (!stack_is_empty() && is_frame_eval) {
            // TODO: if the py stack is empty we have a mismatch.
            frame_t* frame = stack_pop();
            if (has_cframes) {
                while (frame != CFRAME_MAGIC) {
                    mojo_frame_ref(frame);

                    if (stack_is_empty())
                        break;

                    frame = stack_pop();
                }
            } else {
                mojo_frame_ref(frame);
            }
        } else {
            mojo_frame_ref(native_frame);
        }
    }
#ifdef DEBUG
    if (!stack_is_empty()) {
        log_d("Stack mismatch: left with %d Python frames after interleaving", stack_pointer());
    }
#endif
    while (!stack_kernel_is_empty()) {
        char* scope = stack_kernel_pop();
        mojo_frame_kernel(scope);
        free(scope);
    }

#else
    while (!stack_is_empty()) {
        frame_t* frame = stack_pop();
        mojo_frame_ref(frame);
    }
#endif

    if (self->sample_data.gc_state == GC_STATE_COLLECTING) {
        mojo_event(MOJO_GC);
    }

    // Finish off sample with the metric(s)
    sample_t* sample = &self->sample_data;
    if (pargs.full) {
        mojo_metric_time(sample->time);
        if (sample->is_idle) {
            mojo_event(MOJO_IDLE);
        }
        mojo_metric_memory(sample->memory);
    } else {
        if (pargs.memory) {
            mojo_metric_memory(sample->memory);
        } else {
            mojo_metric_time(sample->time);
        }
    }

    // In pipe mode we do Austin event buffering.
    if (pargs.pipe)
        fflush(pargs.output_file);
}

event_handler_t*
mojo_event_handler_new(void) {
    event_handler_t* handler = (event_handler_t*)calloc(1, sizeof(base_event_handler_t));
    if (!isvalid(handler)) {
        log_e("Failed to allocate memory for event handler"); // GCOV_EXCL_START
        return NULL;                                          // GCOV_EXCL_STOP
    }

    handler->spec.emit_stack_begin = (event_handler_stack_begin_t)mojo_event_handler__handle_stack_begin;
    handler->spec.emit_metadata    = (event_handler_metadata_t)mojo_event_handler__handle_metadata;
    handler->spec.emit_new_string  = (event_handler_new_string_t)mojo_event_handler__handle_new_string;
    handler->spec.emit_new_frame   = (event_handler_new_frame_t)mojo_event_handler__handle_new_frame;
    handler->spec.emit_stack_end   = (event_handler_stack_end_t)mojo_event_handler__handle_stack_end;

    mojo_header();

    return handler;
}

// ----------------------------------------------------------------------------
// Where event handler

const char* WHERE_SAMPLE_FORMAT = "    " BYEL "%2$s" CRESET " (" BCYN "%1$s" CRESET ":" BGRN "%3$d" CRESET ")\n";
#ifdef NATIVE
const char* WHERE_SAMPLE_FORMAT_NATIVE
    = "    " HBLK256 "%2$s" CRESET " (" BBLK256 "%1$s" CRESET ":" HBLK256 "%3$d" CRESET ")\n";
const char* WHERE_SAMPLE_FORMAT_KERNEL = "    " BHBLU256 "%s" CRESET " 🐧\n";
#endif
#if defined PL_WIN
const char* WHERE_HEAD_FORMAT
    = "\n\n%4$s Process " BMAG "%1$I64d" CRESET " 🧵 Thread " BBLU "%2$I64d:%3$I64d" CRESET "\n\n";
#else
const char* WHERE_HEAD_FORMAT = "\n\n%4$s Process " BMAG "%1$d" CRESET " 🧵 Thread " BBLU "%2$ld:%3$ld" CRESET "\n\n";
#endif

// ----------------------------------------------------------------------------
static inline void
format_frame_ref(const char* format, frame_t* frame) {
    cached_string_t* scope = frame->scope;
    fprintfp(
        pargs.output_file, format, frame->filename->value, scope == UNKNOWN_SCOPE ? "<unknown>" : scope->value,
        frame->line
    );
}

#ifdef NATIVE
// ----------------------------------------------------------------------------
static inline void
format_kernel_frame_ref(const char* format, char* scope) {
    fprintfp(pargs.output_file, format, scope);
}
#endif

static inline void
where_event_handler__handle_stack_begin(base_event_handler_t* self, sample_t* sample) {
    fprintfp(
        pargs.output_file, WHERE_HEAD_FORMAT, sample->pid, sample->iid, sample->tid, sample->is_idle ? "💤" : "🚀"
    );
}

void
where_event_handler__handle_stack_end(base_event_handler_t* self) {
#ifdef NATIVE
    bool has_cframes = false;
    if (stack_top() == CFRAME_MAGIC) {
        has_cframes = true;
        (void)stack_pop();
    }

    while (!stack_native_is_empty()) {
        frame_t* native_frame = stack_native_pop();
        if (!isvalid(native_frame)) {
            log_e("Invalid native frame"); // GCOV_EXCL_START
            break;                         // GCOV_EXCL_STOP
        }
        cached_string_t* scope = native_frame->scope;
        if (!isvalid(scope)) {
            scope = UNKNOWN_SCOPE; // GCOV_EXCL_LINE
        }

        bool is_frame_eval
            = (scope == UNKNOWN_SCOPE) ? false : isvalid(strstr(scope->value, "PyEval_EvalFrameDefault"));
        if (!stack_is_empty() && is_frame_eval) {
            // TODO: if the py stack is empty we have a mismatch.
            frame_t* frame = stack_pop();
            if (has_cframes) {
                while (frame != CFRAME_MAGIC) {
                    format_frame_ref(WHERE_SAMPLE_FORMAT, frame);

                    if (stack_is_empty())
                        break;

                    frame = stack_pop();
                }
            } else {
                format_frame_ref(WHERE_SAMPLE_FORMAT, frame);
            }
        } else {
            format_frame_ref(WHERE_SAMPLE_FORMAT_NATIVE, native_frame);
        }
    }
#ifdef DEBUG
    if (!stack_is_empty()) {
        log_d("Stack mismatch: left with %d Python frames after interleaving", stack_pointer());
    }
#endif
    while (!stack_kernel_is_empty()) {
        char* scope = stack_kernel_pop();
        format_kernel_frame_ref(WHERE_SAMPLE_FORMAT_KERNEL, scope);
        free(scope);
    }

#else
    while (!stack_is_empty()) {
        frame_t* frame = stack_pop();
        format_frame_ref(WHERE_SAMPLE_FORMAT, frame);
    }
#endif
}

event_handler_t*
where_event_handler_new(void) {
    event_handler_t* handler = (event_handler_t*)calloc(1, sizeof(base_event_handler_t));
    if (!isvalid(handler)) {
        log_e("Failed to allocate memory for event handler"); // GCOV_EXCL_START
        return NULL;                                          // GCOV_EXCL_STOP
    }

    handler->spec.emit_stack_begin = (event_handler_stack_begin_t)where_event_handler__handle_stack_begin;
    handler->spec.emit_stack_end   = (event_handler_stack_end_t)where_event_handler__handle_stack_end;

    return handler;
}
