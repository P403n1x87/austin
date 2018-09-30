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

#include "error.h"
#include "logging.h"
#include "python.h"

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
    if (ts.frame != NULL) {
      raddr_t frame_raddr = { .pid = raddr->pid, .addr = ts.frame };
      py_frame = py_frame_new_from_raddr(&frame_raddr);
      if (py_frame == NULL)
        error = ETHREADNOFRAME;

      else {
        register int limit = FRAME_LIMIT;
        last_frame = py_frame;
        while (py_frame != NULL && limit--) {
          if (py_frame->invalid) {
            error = ETHREADNOFRAME;
            free(last_frame);
            last_frame = NULL;
            break;
          }
          else {
            first_frame = py_frame;
            py_frame = py_frame__prev(py_frame);
          }
        }

        if (last_frame == NULL)
          error = ETHREADNOFRAME;
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
      py_thread->next_raddr.addr = ts.next == raddr->addr ? NULL : ts.next;

      py_thread->tid  = ts.thread_id;
      py_thread->next = NULL;

      py_thread->first_frame = first_frame;
      py_thread->last_frame  = last_frame;

      py_thread->invalid = 0;
    }
  }

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
