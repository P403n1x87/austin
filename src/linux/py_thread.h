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

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#include "../hints.h"
#include "../py_thread.h"
#include "../resources.h"


// ----------------------------------------------------------------------------
static int
_py_thread__is_idle(py_thread_t * self) {
  char file_name[64];
  char buffer[2048] = "";

  sprintf(file_name, "/proc/%d/task/" SIZE_FMT "/stat", self->proc->pid, self->tid);
  
  cu_fd fd = open(file_name, O_RDONLY);
  if (fd == -1) {
    log_d("Cannot open %s", file_name);
    return -1;
  }

  if (read(fd, buffer, 2047) == 0) {
    log_d("Cannot read %s", file_name);
    return -1;
  }

  char * p = strchr(buffer, ')');
  if (!isvalid(p)) {
    log_d("Invalid format for procfs file %s", file_name);
    return -1;
  }

  p+=2;
  if (*p == ' ')
    p++;

  return (*p != 'R');
}
