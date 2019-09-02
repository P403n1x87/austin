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

#include <string.h>

#include "argparse.h"
#include "error.h"
#include "logging.h"
#include "version.h"

#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

#define FRAME_LIMIT                 4096


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_thread_t *
py_thread_new_from_raddr(raddr_t * raddr) {
  PyThreadState   ts;
  py_frame_t    * py_frame    = NULL;
  py_thread_t   * py_thread   = NULL;
  py_frame_t    * first_frame = NULL;
  py_frame_t    * last_frame  = NULL;

  error = EOK;

  if (copy_from_raddr(raddr, ts) != sizeof(ts))
    error = ETHREAD;

  else {
    if (V_FIELD(void*, ts, py_thread, o_frame) != NULL) {
      raddr_t frame_raddr = { .pid = raddr->pid, .addr = V_FIELD(void*, ts, py_thread, o_frame) };
      py_frame = py_frame_new_from_raddr(&frame_raddr);
      if (py_frame == NULL)
        error = ETHREADNOFRAME;

      else {
        register int limit = FRAME_LIMIT;
        last_frame = py_frame;
        while (py_frame != NULL && --limit) {
          if (py_frame->invalid) {
            error = ETHREADNOFRAME;
            py_frame__destroy(last_frame);
            last_frame = NULL;
            break;
          }

          first_frame = py_frame;

          py_frame = py_frame__prev(py_frame);
        }
        if (!limit)
          log_w("Frames limit reached. Discarding the rest");
      }
    }
  }

  if ((error & ETHREAD) == 0) {
    py_thread = (py_thread_t *) malloc(sizeof(py_thread_t));
    if (py_thread == NULL)
      error = ETHREAD;

    else {
      py_thread->raddr.pid  = raddr->pid;
      py_thread->raddr.addr = raddr->addr;

      py_thread->next_raddr.pid  = raddr->pid;
      py_thread->next_raddr.addr = V_FIELD(void*, ts, py_thread, o_next) == raddr->addr \
        ? NULL \
        : V_FIELD(void*, ts, py_thread, o_next);

      py_thread->tid  = V_FIELD(long, ts, py_thread, o_thread_id);
      if (py_thread->tid == 0)
        py_thread->tid = (long) raddr->addr;
      py_thread->next = NULL;

      py_thread->first_frame = first_frame;
      py_thread->last_frame  = last_frame;

      py_thread->invalid = 0;
    }
  }

  if (py_thread == NULL && last_frame != NULL)
    py_frame__destroy(last_frame);

  check_not_null(py_thread);
  return py_thread;
}


// ----------------------------------------------------------------------------
py_frame_t *
py_thread__first_frame(py_thread_t * self) {
  if (self == NULL)
    return NULL;

  return self->first_frame;
}


// ----------------------------------------------------------------------------
py_thread_t *
py_thread__next(py_thread_t * self) {
  if (self == NULL || self->next_raddr.addr == NULL)
    return NULL;

  if (self->next == NULL) {
    self->next = py_thread_new_from_raddr(&(self->next_raddr));
    if (self->next == NULL) {
      self->invalid = 1;
      error = ETHREADINV;
    }
  }

  check_not_null(self->next);
  return self->next;
}


// ----------------------------------------------------------------------------
int
py_thread__print_collapsed_stack(py_thread_t * thread, ctime_t delta, ssize_t mem_delta) {
  if (!pargs.full && pargs.memory && mem_delta <= 0)
    return 1;
  
  if (thread->invalid) {
    fprintf(pargs.output_file, "Thread %lx;Bad sample %ld\n", thread->tid, delta);
    stats_count_error();
    return 0;
  }

  py_frame_t * frame = py_thread__first_frame(thread);

  if (frame == NULL && pargs.exclude_empty)
    // Skip if thread has no frames and we want to exclude empty threads
    return 1;

  // Group entries by thread.
  fprintf(pargs.output_file, "Thread %lx", thread->tid);

  // Append frames
  while(frame != NULL) {
    py_code_t * code = frame->code;
    if (pargs.sleepless && strstr(code->scope, "wait") != NULL) {
      delta = 0;
      fprintf(pargs.output_file, ";<idle>");
      break;
    }
    fprintf(pargs.output_file, pargs.format, code->scope, code->filename, code->lineno);

    frame = frame->next;
  }

  // Finish off sample with the metric(s)
  if (pargs.full) {
    fprintf(pargs.output_file, " %lu %ld %ld\n",
      delta, mem_delta >= 0 ? mem_delta : 0, mem_delta < 0 ? mem_delta : 0
    );
  }
  else {
    if (pargs.memory)
      fprintf(pargs.output_file, " %ld\n", mem_delta);
    else
      fprintf(pargs.output_file, " %lu\n", delta);
  }

  return 0;
}


// ----------------------------------------------------------------------------
void
py_thread__destroy(py_thread_t * self) {
  if (self == NULL)
    return;

  // Destroy frame list
  if (self->last_frame != NULL)
    py_frame__destroy(self->last_frame);

  // Destroy next thread state
  if (self->next != NULL)
    py_thread__destroy(self->next);

  free(self);
}
