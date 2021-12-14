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

#include "../hints.h"
#include "../py_thread.h"


// Support for CPU time on Linux. We need to retrieve the TID from the the
// struct pthread pointed to by the native thread ID stored by Python. We do not
// have the definition of the structure, so we need to "guess" the offset of the
// tid field within struct pthread.
#define PTHREAD_BUFFER_SIZE          200


static int _pthread_tid_offset = 0;
void * _pthread_buffer[PTHREAD_BUFFER_SIZE];


// ----------------------------------------------------------------------------
static void
_infer_tid_field_offset(py_thread_t * py_thread) {
  if (fail(copy_memory(
      py_thread->raddr.pid,
      (void *) py_thread->tid,  // At this point this is still the pthread_t *
      PTHREAD_BUFFER_SIZE * sizeof(void *),
      _pthread_buffer
  ))) {
    log_d("Cannot copy pthread_t structure");
    return;
  }

  log_d("pthread_t at %p", py_thread->tid);

  for (register int i = 0; i < PTHREAD_BUFFER_SIZE; i++) {
    if (py_thread->raddr.pid == (uintptr_t) _pthread_buffer[i]) {
      log_d("TID field offset: %d", i);
      _pthread_tid_offset = i;
      return;
    }
  }

  // Fall-back to smaller steps if we failed
  for (register int i = 0; i < PTHREAD_BUFFER_SIZE * sizeof(uintptr_t) / sizeof(pid_t); i++) {
    if (py_thread->raddr.pid == (pid_t) ((pid_t*) _pthread_buffer)[i]) {
      log_d("TID field offset (from fall-back): %d", i);
      _pthread_tid_offset = i;
      return;
    }
  }
}


// ----------------------------------------------------------------------------
static int
_py_thread__is_idle(py_thread_t * self) {
  with_resources;

  char      file_name[64];
  char      buffer[2048];

  retval = -1;

  sprintf(file_name, "/proc/%d/task/%ld/stat", self->raddr.pid, self->tid);
  int fd = open(file_name, O_RDONLY);
  if (fd == -1) {
    log_d("Cannot open %s", file_name);
    return -1;
  }

  if (read(fd, buffer, 2047) == 0) {
    log_d("Cannot read %s", file_name);
    goto release;
  }

  char * p = strchr(buffer, ')') + 2;
  if (p == NULL) {
    log_d("Invalid format for procfs file %s", file_name);
    goto release;
  }
  if (p[0] == ' ') ++p;
  retval = p[0] != 'R';

release:
  close(fd);
  released;
}
