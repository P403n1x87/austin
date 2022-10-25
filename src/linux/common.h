// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2021 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <sys/ptrace.h>

#include "../stats.h"


#define PTHREAD_BUFFER_ITEMS  200


#if defined __arm__
#define ADDR_FMT "%x"
#define SIZE_FMT "%d"
#else
#define ADDR_FMT "%lx"
#define SIZE_FMT "%ld"
#endif


static uintptr_t _pthread_buffer[PTHREAD_BUFFER_ITEMS];

#define read_pthread_t(pid, addr) \
  (copy_memory(pid, addr, sizeof(_pthread_buffer), _pthread_buffer))


struct _proc_extra_info {
  unsigned int page_size;
  char         statm_file[24];
  pthread_t    wait_thread_id;
  unsigned int pthread_tid_offset;
};


#ifdef NATIVE
#include <sched.h>

static inline int
wait_ptrace(enum __ptrace_request request, pid_t pid, void * addr, void * data) {
  int outcome = 0;
  ctime_t end = gettime() + 100000;  // Wait for 100ms
  
  while (gettime() < end && (outcome = ptrace(request, pid, addr, data)) && errno == 3)
    sched_yield();

  #ifdef DEBUG
  ctime_t wait = gettime() - end + 100000;
  if (wait > 1000)
    log_d("ptrace long wait for request %d: %ld microseconds", request, wait);
  #endif
  
  return outcome;
}

#endif


#endif
