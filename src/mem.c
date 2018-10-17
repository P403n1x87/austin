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

#include "mem.h"
#include "logging.h"

ssize_t
copy_memory(pid_t pid, void * addr, ssize_t len, void * buf) {
  #if defined(__linux__)

  struct iovec local[1];
  struct iovec remote[1];

  local[0].iov_base = buf;
  local[0].iov_len = len;
  remote[0].iov_base = addr;
  remote[0].iov_len = len;

  return process_vm_readv(pid, local, 1, remote, 1, 0);

  #elif defined(_WIN32) || defined(_WIN64)

  size_t n;
  int ret = ReadProcessMemory((HANDLE) pid, addr, buf, len, &n) ? n : -1;
  return ret;

  #elif defined(__APPLE__) && defined(__MACH__)

  // Get task from pid
  // Allocate
  // Read
  // Deallocate

  #endif
}
