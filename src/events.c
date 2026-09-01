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

#include <string.h>

#include "ansi.h"
#include "argparse.h"
#include "events.h"
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
    char thread_name[128];

    base_event_handler__handle_stack_begin(self, sample);

    if (sample->thread_name && sample->thread_name[0])
        snprintf(thread_name, sizeof(thread_name), "%s", sample->thread_name);
    else
        sprintf(thread_name, FORMAT_TID, sample->tid); // GCOV_EXCL_LINE

    bool is_iid_negative = sample->iid < 0;
    mojo_event(MOJO_STACK);
    mojo_integer(sample->pid, 0);
    mojo_integer(is_iid_negative ? -sample->iid : sample->iid, is_iid_negative);
    mojo_string(thread_name);
}

static inline void
mojo_event_handler__handle_metadata(base_event_handler_t* self, char* key, char* value, va_list args) {
    _mojo_flush();
    mojo_event(MOJO_METADATA);
    mojo_string(key);
    _mojo_flush();
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
    bool py_repeat = stack_top() == PYSTACK_REPEAT_MAGIC;
    if (py_repeat) {
        (void)stack_pop();
        mojo_stack_repeat();
    }

    bool has_cframes = false;
    if (stack_top() == CFRAME_MAGIC) {
        has_cframes = true;
        (void)stack_pop();
    }

    if (pargs_native) {
        while (!stack_native_is_empty()) {
            frame_t* native_frame = stack_native_pop();
            if (!isvalid(native_frame)) {
                log_e("Invalid native frame"); // GCOV_EXCL_START
                break;                         // GCOV_EXCL_STOP
            }

            if (py_repeat) {
                // Python stack is repeated; native frames above the eval boundary
                // are always emitted unconditionally.
                mojo_frame_ref(native_frame);
                continue;
            }

            if (native_frame == (frame_t*)EVAL_FRAME_MAGIC) {
                if (!stack_is_empty()) {
                    frame_t* frame = stack_pop();
                    if (has_cframes) {
                        while (frame != CFRAME_MAGIC) {
                            mojo_frame_ref(frame);

                            if (stack_is_empty())
                                break;

                            frame = stack_pop();
                        }
                    } else {
                        if (frame != CFRAME_MAGIC) {
                            mojo_frame_ref(frame);
                        }
                    }
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
    }

    // In non-native mode the native stack is always empty so the interleaving
    // loop above never runs.  Drain Python frames directly in that case.
    else {
        while (!stack_is_empty()) {
            frame_t* frame = stack_pop();
            if (frame != CFRAME_MAGIC) {
                mojo_frame_ref(frame);
            }
        }
    }

    // Finish off sample with the metric(s)
    mojo_emit_metrics(&self->sample_data);

    _mojo_flush();

    // In pipe mode we do Austin event buffering.
    if (pargs.pipe)
        fflush(pargs.output_file);
}

// ----------------------------------------------------------------------------
// Suspended-task introspection (3.14+ asyncio): fires only when a task's
// coroutine frame has changed since the last scan, emitting a pure Python
// frame sequence (no GC/idle flags, no native-frame interleaving)
// terminated by a single MOJO_METRIC_TIME. That metric is always a time
// value, skipped entirely in pure memory mode, and describes the task's
// PREVIOUS suspension point, not the frames in this same record.

static inline void
mojo_event_handler__handle_task_stack_begin(base_event_handler_t* self, uintptr_t task_id, uintptr_t name_key) {
    mojo_task_stack(task_id, name_key);
}

static inline void
mojo_event_handler__handle_task_stack_end(base_event_handler_t* self, uint64_t elapsed) {
    while (!task_stack_is_empty()) {
        frame_t* frame = task_stack_pop();
        if (frame != CFRAME_MAGIC) {
            mojo_frame_ref(frame);
        }
    }

    mojo_metric_time(elapsed);

    _mojo_flush();

    if (pargs.pipe)
        fflush(pargs.output_file);
}

static inline void
mojo_event_handler__handle_task_waiter(base_event_handler_t* self, uintptr_t task_id, uintptr_t waiter_id) {
    mojo_task_waiter(task_id, waiter_id);
    _mojo_flush();

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

    handler->spec.emit_task_stack_begin = (event_handler_task_stack_begin_t)mojo_event_handler__handle_task_stack_begin;
    handler->spec.emit_task_stack_end   = (event_handler_task_stack_end_t)mojo_event_handler__handle_task_stack_end;
    handler->spec.emit_task_waiter      = (event_handler_task_waiter_t)mojo_event_handler__handle_task_waiter;

    mojo_header();

    return handler;
}

// ----------------------------------------------------------------------------
// Where event handler

const char* WHERE_SAMPLE_FORMAT = "    " BYEL "%2$s" CRESET " (" BCYN "%1$s" CRESET ":" BGRN "%3$d" CRESET ")\n";
const char* WHERE_SAMPLE_FORMAT_NATIVE
    = "    " HBLK256 "%2$s" CRESET " (" BBLK256 "%1$s" CRESET ":" HBLK256 "%3$d" CRESET ")\n";
const char* WHERE_SAMPLE_FORMAT_KERNEL = "    " BHBLU256 "%s" CRESET " 🐧\n";
#if defined PL_WIN
const char* WHERE_HEAD_FORMAT
    = "\n\n%4$s Process " BMAG "%1$I64d" CRESET " 🧵 Thread " BBLU "%2$I64d:%3$s" CRESET "\n\n";
#else
const char* WHERE_HEAD_FORMAT = "\n\n%4$s Process " BMAG "%1$d" CRESET " 🧵 Thread " BBLU "%2$ld:%3$s" CRESET "\n\n";
#endif

// ----------------------------------------------------------------------------
static inline void
format_frame_ref(const char* format, frame_t* frame) {
    cached_string_t* scope = frame->scope;
    fprintfp(pargs.output_file, format, frame->filename->value, scope->value, frame->line);
}

// ----------------------------------------------------------------------------
static inline void
format_kernel_frame_ref(const char* format, char* scope) {
    fprintfp(pargs.output_file, format, scope);
}

// ----------------------------------------------------------------------------
// Where handler: suspended-task tree, rendered as a compact indented tree. A
// tree's indentation and branch connectors depend on knowing the whole waiter
// graph up front, so everything below is buffered while sampling runs, then
// printed right after the stack dump of the thread that owns it, with a
// standalone section at the end for tasks with no owning thread at all
// (orphaned).
//
// Bounded, fixed-size storage throughout -- this is a one-shot diagnostic
// snapshot, not a long-running collector, so a generous fixed cap costs nothing
// in practice and avoids realloc bookkeeping entirely.

#define WHERE_MAX_TASKS       1024
#define WHERE_MAX_TASK_FRAMES 64
#define WHERE_MAX_TASK_EDGES  2048
#define WHERE_MAX_STRINGS     4096
#define WHERE_MAX_TREE_DEPTH  64 // guards against a corrupted/cyclic waiter graph in remote memory
#define WHERE_PREFIX_MAX      2048

typedef struct {
    key_dt key;
    char*  value;
} where_string_t;

typedef struct {
    uintptr_t task_id;
    char*     name;                          // resolved via the string table; NULL if unresolved
    char*     frames[WHERE_MAX_TASK_FRAMES]; // formatted "scope (file:line)", root first
    size_t    n_frames;
} where_task_t;

typedef struct {
    uintptr_t task_id;   // this task...
    uintptr_t waiter_id; // ...is being awaited by this one
} where_task_edge_t;

typedef struct {
    event_handler_spec_t spec;
    sample_t             sample_data;

    where_string_t strings[WHERE_MAX_STRINGS];
    size_t         n_strings;

    where_task_t tasks[WHERE_MAX_TASKS];
    size_t       n_tasks;
    bool         has_current_task;
    size_t       current_task; // index into tasks[], valid only while has_current_task

    // self->tasks[] accumulates across threads in contiguous runs -- orphans
    // first, then each thread's own tasks in scan order. task_base marks
    // where the current run starts, so render_tree only scans that slice.
    // orphan_count is the size of the orphan run, captured once on the
    // first thread scan.
    size_t task_base;
    size_t orphan_count;
    bool   orphan_count_set;

    where_task_edge_t edges[WHERE_MAX_TASK_EDGES];
    size_t            n_edges;
} where_event_handler_t;

// ----------------------------------------------------------------------------
// Portable strdup -- not every supported platform's libc exposes strdup
// itself (e.g. MSVC calls it _strdup), so a tiny local helper sidesteps the
// portability question entirely rather than adding a per-platform #ifdef.
static char*
where_strdup(const char* s) {
    size_t len  = strlen(s) + 1;
    char*  copy = (char*)malloc(len);
    if (isvalid(copy))
        memcpy(copy, s, len);
    return copy;
}

// ----------------------------------------------------------------------------
static void
where_event_handler__handle_new_string(where_event_handler_t* self, cached_string_t* string) {
    if (self->n_strings >= WHERE_MAX_STRINGS)
        return; // best-effort: an over-cap string just won't resolve by name later

    self->strings[self->n_strings].key   = string->key;
    self->strings[self->n_strings].value = string->value; // owned by the LRU cache; lives for the process's life
    self->n_strings++;
}

// ----------------------------------------------------------------------------
static const char*
where_event_handler__lookup_string(where_event_handler_t* self, key_dt key) {
    for (size_t i = 0; i < self->n_strings; i++) {
        if (self->strings[i].key == key)
            return self->strings[i].value;
    }
    return NULL;
}

// ----------------------------------------------------------------------------
static void
where_event_handler__handle_task_stack_begin(where_event_handler_t* self, uintptr_t task_id, uintptr_t name_key) {
    if (self->n_tasks >= WHERE_MAX_TASKS) {
        self->has_current_task = false;
        return;
    }

    where_task_t* task = &self->tasks[self->n_tasks];
    task->task_id      = task_id;
    task->n_frames     = 0;

    const char* name = where_event_handler__lookup_string(self, name_key);
    task->name       = isvalid(name) ? where_strdup(name) : NULL;

    self->current_task     = self->n_tasks;
    self->has_current_task = true;
    self->n_tasks++;
}

// ----------------------------------------------------------------------------
static void
where_event_handler__handle_task_stack_end(where_event_handler_t* self, uint64_t elapsed) {
    (void)elapsed; // the tree shows structure, not timing -- matches CPython's own pstree

    where_task_t* task = self->has_current_task ? &self->tasks[self->current_task] : NULL;

    while (!task_stack_is_empty()) {
        frame_t* frame = task_stack_pop();
        if (frame == CFRAME_MAGIC || !isvalid(task) || task->n_frames >= WHERE_MAX_TASK_FRAMES)
            continue;

        char buf[256];
        snprintf(buf, sizeof(buf), "%s (%s:%d)", frame->scope->value, frame->filename->value, frame->line);
        task->frames[task->n_frames++] = where_strdup(buf);
    }

    self->has_current_task = false;
}

// ----------------------------------------------------------------------------
static void
where_event_handler__handle_task_waiter(where_event_handler_t* self, uintptr_t task_id, uintptr_t waiter_id) {
    if (self->n_edges >= WHERE_MAX_TASK_EDGES)
        return; // best-effort: an over-cap edge just won't show as a branch later

    self->edges[self->n_edges].task_id   = task_id;
    self->edges[self->n_edges].waiter_id = waiter_id;
    self->n_edges++;
}

// ----------------------------------------------------------------------------
static where_task_t*
where_event_handler__find_task(where_event_handler_t* self, uintptr_t task_id) {
    for (size_t i = 0; i < self->n_tasks; i++) {
        if (self->tasks[i].task_id == task_id)
            return &self->tasks[i];
    }
    return NULL; // best-effort: a waiter edge naming a task we never captured is just omitted from the tree
}

// ----------------------------------------------------------------------------
// True if some other tracked task is awaiting task_id -- i.e. task_id has a
// parent in the display tree and so isn't a root.
static bool
where_event_handler__has_parent(where_event_handler_t* self, uintptr_t task_id) {
    for (size_t i = 0; i < self->n_edges; i++) {
        if (self->edges[i].task_id == task_id)
            return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Compact connectors -- a corner character plus one space, no horizontal
// dash -- rather than pstree(1)'s wider "├── "/"└── ". The continuation
// segments (used to extend the prefix for whatever nests one level deeper)
// match this width exactly, so the tree still lines up correctly.
static void
where_event_handler__print_branch(const char* prefix, bool is_last, const char* label) {
    fprintfp(pargs.output_file, "%s%s%s\n", prefix, is_last ? "└ " : "├ ", label);
}

// ----------------------------------------------------------------------------
// A task's own coroutine-chain frames print like regular thread frames do
// (WHERE_SAMPLE_FORMAT) -- plain, no "├"/"└" -- since only task-to-task
// nesting is a real branch worth marking. Still needs a "│" rather than a
// blank when something follows at this same level (always true here: a
// task's frames are printed before its child tasks, so if it has any
// children, the frames must show the stem continuing down to them --
// otherwise the child tasks below look disconnected from the frame chain
// that led to them, rather than clearly nested under the same task).
static void
where_event_handler__print_frame(const char* prefix, bool has_more, const char* label) {
    fprintfp(pargs.output_file, "%s%s%s\n", prefix, has_more ? "│ " : "  ", label);
}

// ----------------------------------------------------------------------------
// Appends `segment` after prefix's current content (bounded by
// WHERE_PREFIX_MAX), returning the ORIGINAL length so the caller can
// truncate back to it once done printing whatever used the extended prefix.
// Lets one shared buffer be reused across sibling subtrees, each restoring
// it when their own recursion returns, without an allocation per tree level.
static size_t
where_event_handler__extend_prefix(char* prefix, const char* segment) {
    size_t len    = strlen(prefix);
    size_t seglen = strlen(segment);
    if (len + seglen < WHERE_PREFIX_MAX - 1)
        memcpy(prefix + len, segment, seglen + 1); // +1 for the NUL terminator
    return len;
}

static void
where_event_handler__print_task(where_event_handler_t* self, where_task_t* task, char* prefix, bool is_last, int depth);

// ----------------------------------------------------------------------------
// Prints task's own coroutine chain -- one frame per nesting level, matching
// CPython's `python -m asyncio pstree` treating each stack entry as its own
// tree level -- followed by every task it is itself awaiting, recursively.
// `depth` guards against a corrupted or cyclic waiter graph in remote
// memory; a real await chain never comes anywhere close to
// WHERE_MAX_TREE_DEPTH.
static void
where_event_handler__print_task_body(where_event_handler_t* self, where_task_t* task, char* prefix, int depth) {
    if (depth >= WHERE_MAX_TREE_DEPTH)
        return;

    where_task_t* children[WHERE_MAX_TASKS];
    size_t        n_children = 0;
    for (size_t i = 0; i < self->n_edges && n_children < WHERE_MAX_TASKS; i++) {
        if (self->edges[i].waiter_id != task->task_id)
            continue;
        where_task_t* child = where_event_handler__find_task(self, self->edges[i].task_id);
        if (isvalid(child))
            children[n_children++] = child;
    }

    // Only task-to-task nesting indents (handled by print_task's own
    // extend_prefix call below); a task's own coroutine chain is printed
    // flat, at this same single level, one line per frame -- no reason to
    // walk the reader six levels to the right just to say "and then this
    // called that", and no "├"/"└" either, matching how a regular thread's
    // own frames print (WHERE_SAMPLE_FORMAT) with no connector at all. Since
    // frames always print before any child tasks, "more follows" here just
    // means "this task has at least one child".
    for (size_t i = 0; i < task->n_frames; i++)
        where_event_handler__print_frame(prefix, n_children > 0, task->frames[i]);

    for (size_t i = 0; i < n_children; i++)
        where_event_handler__print_task(self, children[i], prefix, i == n_children - 1, depth + 1);
}

// ----------------------------------------------------------------------------
static void
where_event_handler__print_task(
    where_event_handler_t* self, where_task_t* task, char* prefix, bool is_last, int depth
) {
    char label[300];
    snprintf(label, sizeof(label), "🔹 %s", isvalid(task->name) ? task->name : "<unnamed>");
    where_event_handler__print_branch(prefix, is_last, label);

    size_t restore = where_event_handler__extend_prefix(prefix, is_last ? "  " : "│ ");
    where_event_handler__print_task_body(self, task, prefix, depth + 1);
    prefix[restore] = '\0';
}

// ----------------------------------------------------------------------------
// A root task (nobody awaits it) prints with no connector at all -- there's
// nothing above it in the tree for a "├"/"└" to relate it to, so drawing one
// is just decoration. Its own body uses the SAME prefix unchanged (no
// extend_prefix call), one indent level shallower than a non-root task's
// body would be.
static void
where_event_handler__print_root_task(where_event_handler_t* self, where_task_t* task, char* prefix) {
    fprintfp(pargs.output_file, "%s🔹 %s\n", prefix, isvalid(task->name) ? task->name : "<unnamed>");
    where_event_handler__print_task_body(self, task, prefix, 0);
}

// ----------------------------------------------------------------------------
// Renders the task tree in tasks[base, end): every root task in that slice,
// and everything it transitively awaits, printed at the current output
// position.
static void
where_event_handler__render_tree(where_event_handler_t* self, size_t base, size_t end) {
    where_task_t* roots[WHERE_MAX_TASKS];
    size_t        n_roots = 0;
    for (size_t i = base; i < end && n_roots < WHERE_MAX_TASKS; i++) {
        if (!where_event_handler__has_parent(self, self->tasks[i].task_id))
            roots[n_roots++] = &self->tasks[i];
    }

    // Best-effort fallback: if every task in this slice appears to have a
    // parent (a torn read produced a bogus edge, or a genuine cycle),
    // there's no true root to start from -- fall back to printing each of
    // them as its own top-level tree rather than silently showing nothing.
    if (n_roots == 0) {
        for (size_t i = base; i < end && n_roots < WHERE_MAX_TASKS; i++)
            roots[n_roots++] = &self->tasks[i];
    }

    // Starts at the same 4-space indent as a regular thread frame's own
    // lines (WHERE_SAMPLE_FORMAT), so the root task reads as nested under
    // the owning frame it's printed beneath, not as its own unrelated
    // top-level section starting back at column 0.
    char prefix[WHERE_PREFIX_MAX];
    snprintf(prefix, sizeof(prefix), "    ");
    for (size_t i = 0; i < n_roots; i++)
        where_event_handler__print_root_task(self, roots[i], prefix);
}

static inline void
where_event_handler__handle_stack_begin(where_event_handler_t* self, sample_t* sample) {
    // Recorded (unlike a plain base_event_handler_t user) so handle_stack_end
    // can print this thread's own task tree (if it owns any) right after
    // this thread's own frames instead of needing a separate section at the
    // end.
    self->sample_data = *sample;

    // Whatever's in tasks[] on the first call is exactly the orphan set;
    // captured once, then every later call just updates task_base.
    if (!self->orphan_count_set) {
        self->orphan_count     = self->n_tasks;
        self->orphan_count_set = true;
    }
    self->task_base = self->n_tasks;

    char tid_buf[32];
    if (!sample->thread_name || !sample->thread_name[0])
        sprintf(tid_buf, FORMAT_TID, sample->tid); // GCOV_EXCL_LINE

    const char* tname
        = (sample->thread_name && sample->thread_name[0]) ? sample->thread_name : tid_buf; // GCOV_EXCL_BR_LINE
    fprintfp(pargs.output_file, WHERE_HEAD_FORMAT, sample->pid, sample->iid, tname, sample->is_idle ? "💤" : "🚀");
}

void
where_event_handler__handle_stack_end(where_event_handler_t* self) {
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
        if (native_frame == (frame_t*)EVAL_FRAME_MAGIC) {
            // TODO: if the py stack is empty we have a mismatch.
            if (!stack_is_empty()) {
                frame_t* frame = stack_pop();
                if (has_cframes) {
                    while (frame != CFRAME_MAGIC) {
                        format_frame_ref(WHERE_SAMPLE_FORMAT, frame);

                        if (stack_is_empty())
                            break;

                        frame = stack_pop();
                    }
                } else {
                    if (frame != CFRAME_MAGIC) {
                        format_frame_ref(WHERE_SAMPLE_FORMAT, frame);
                    }
                }
            }
        } else {
            format_frame_ref(WHERE_SAMPLE_FORMAT_NATIVE, native_frame);
        }
    }

    // In non-native mode the native stack is always empty so the interleaving
    // loop above never runs.  Drain Python frames directly in that case.
    if (!pargs_native) {
        while (!stack_is_empty()) {
            frame_t* frame = stack_pop();
            if (frame != CFRAME_MAGIC) {
                format_frame_ref(WHERE_SAMPLE_FORMAT, frame);
            }
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

    // Render whatever task tree this thread owns, if any -- a no-op (prints
    // nothing) when it owns none, so there's no need to gate this on
    // anything first.
    where_event_handler__render_tree(self, self->task_base, self->n_tasks);
}

// ----------------------------------------------------------------------------
void
where_event_handler__destroy(where_event_handler_t* self) {
    // If we detected any tasks with no live owning thread, render them as a
    // separate task tree at the end.
    if (self->orphan_count > 0) {
        fprintfp(pargs.output_file, "\n\n🌳 Orphaned task tree\n\n");
        where_event_handler__render_tree(self, 0, self->orphan_count);
    }

    // task->name and every frame in task->frames[] are heap-allocated via
    // where_strdup (see where_event_handler__handle_task_stack_begin/_end);
    // self->strings[].value is NOT ours to free (owned by the LRU cache, see
    // where_event_handler__handle_new_string).
    for (size_t i = 0; i < self->n_tasks; i++) {
        where_task_t* task = &self->tasks[i];
        free(task->name);
        for (size_t j = 0; j < task->n_frames; j++)
            free(task->frames[j]);
    }
}

event_handler_t*
where_event_handler_new(void) {
    where_event_handler_t* handler = (where_event_handler_t*)calloc(1, sizeof(where_event_handler_t));
    if (!isvalid(handler)) {
        log_e("Failed to allocate memory for event handler"); // GCOV_EXCL_START
        return NULL;                                          // GCOV_EXCL_STOP
    }

    handler->has_current_task = false;

    handler->spec.emit_stack_begin = (event_handler_stack_begin_t)where_event_handler__handle_stack_begin;
    handler->spec.emit_stack_end   = (event_handler_stack_end_t)where_event_handler__handle_stack_end;
    handler->spec.emit_new_string  = (event_handler_new_string_t)where_event_handler__handle_new_string;

    handler->spec.emit_task_stack_begin
        = (event_handler_task_stack_begin_t)where_event_handler__handle_task_stack_begin;
    handler->spec.emit_task_stack_end = (event_handler_task_stack_end_t)where_event_handler__handle_task_stack_end;
    handler->spec.emit_task_waiter    = (event_handler_task_waiter_t)where_event_handler__handle_task_waiter;

    handler->spec.destroy = (event_handler_destroy_t)where_event_handler__destroy;

    return (event_handler_t*)handler;
}
