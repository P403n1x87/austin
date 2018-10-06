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
#include "py_frame.h"
#include "python.h"


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_frame_t *
py_frame_new_from_raddr(raddr_t * raddr) {
  PyFrameObject   frame;
  py_code_t     * py_code  = NULL;
  py_frame_t    * py_frame = NULL;

  if (copy_from_raddr(raddr, frame) != sizeof(frame))
    error = EFRAME;

  else {
    raddr_t py_code_raddr = { .pid = raddr->pid, .addr = frame.f_code };
    py_code = py_code_new_from_raddr(&py_code_raddr, frame.f_lasti);
    if (py_code == NULL)
      error = EFRAMENOCODE;

    else {
      py_frame = (py_frame_t *) malloc(sizeof(py_frame_t));
      if (py_frame == NULL)
        error = EFRAME;

      else {
        py_frame->raddr.pid  = raddr->pid;
        py_frame->raddr.addr = raddr->addr;

        py_frame->prev_raddr.pid  = raddr->pid;
        py_frame->prev_raddr.addr = frame.f_back;

        py_frame->frame_no = 0;
        py_frame->prev     = NULL;
        py_frame->next     = NULL;

        py_frame->code = py_code;

        py_frame->invalid = 0;
      }
    }
  }

  if (py_frame == NULL && py_code != NULL)
    py_code__destroy(py_code);

  check_not_null(py_frame);
  return py_frame;
}


// ----------------------------------------------------------------------------
py_frame_t *
py_frame__prev(py_frame_t * self) {
  if (self == NULL || self->prev_raddr.addr == NULL)
    return NULL;

  if (self->prev == NULL) {
    // Lazy-loading
    self->prev = py_frame_new_from_raddr(&(self->prev_raddr));
    if (self->prev == NULL) {
      self->invalid = 1;
      error = EFRAMEINV;
    }
    else {
      self->prev->frame_no = self->frame_no + 1;
      self->prev->next     = self;
    }
  }

  check_not_null(self->prev);
  return self->prev;
}


// ----------------------------------------------------------------------------
void
py_frame__destroy(py_frame_t * self) {
  if (self == NULL)
    return;

  if (self->code != NULL)
    py_code__destroy(self->code);

  if (self->prev != NULL) {
    self->prev->next = NULL;
    py_frame__destroy(self->prev);
  }

  if (self->next != NULL) {
    self->next->prev = NULL;
    py_frame__destroy(self->next);
  }

  free(self);
}
