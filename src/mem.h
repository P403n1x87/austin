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

#ifndef MEM_H
#define MEM_H


#include <sys/types.h>

#include "platform.h"

#if defined PL_LINUX
  #include <sys/uio.h>
  ssize_t process_vm_readv(
    pid_t, const struct iovec *, unsigned long liovcnt,
    const struct iovec *remote_iov, unsigned long riovcnt, unsigned long flags
  );

#elif defined(PL_WIN)
  #include <windows.h>

#elif defined(PL_MACOS)
  #include <mach/mach.h>
  #include <mach/mach_vm.h>
  #include <mach/machine/kern_return.h>

#endif

#include "logging.h"

#define OUT_OF_BOUND                  -1


/**
 * Copy a data structure from the given remote address structure.
 * @param  raddr the remote address
 * @param  dt    the data structure as a local variable
 * @return       the number of bytes read.
 */
#define copy_from_raddr(raddr, dt) copy_memory(raddr->pid, raddr->addr, sizeof(dt), &dt)


/**
 * Copy a data structure from the given remote address structure.
 * @param  raddr the remote address
 * @param  dt    the data structure as a local variable
 * @return       the number of bytes read.
 */
#define copy_from_raddr_v(raddr, dt, n) copy_memory(raddr->pid, raddr->addr, n, &dt) != n


/**
 * Same as copy_from_raddr, but with explicit arguments instead of a pointer to
 * a remote address structure
 * @param  pid  the process ID
 * @param  addr the remote address
 * @param  dt   the data structure as a local variable.
 * @return      the number of bytes read.
 */
#define copy_datatype(pid, addr, dt) copy_memory(pid, addr, sizeof(dt), &dt)


typedef struct {
  pid_t  pid;
  void * addr;
} raddr_t;


/**
 * Copy a chunk of memory from a portion of the virtual memory of another
 * process.
 * @param pid_t   the process ID
 * @param void *  the remote address
 * @param ssize_t the number of bytes to read
 * @param void *  the destination buffer, expected to be at least as large as
 *                the number of bytes to read.
 */
static inline ssize_t
copy_memory(pid_t pid, void * addr, ssize_t len, void * buf) {
  #if defined(PL_LINUX)                                              /* LINUX */
  struct iovec local[1];
  struct iovec remote[1];

  local[0].iov_base = buf;
  local[0].iov_len = len;
  remote[0].iov_base = addr;
  remote[0].iov_len = len;

  return process_vm_readv(pid, local, 1, remote, 1, 0);

  #elif defined(PL_WIN)                                                /* WIN */
  size_t n;
  int ret = ReadProcessMemory((HANDLE) pid, addr, buf, len, &n) ? n : -1;
  return ret;

  #elif defined(PL_MACOS)                                              /* MAC */
  mach_port_t task;
  if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
    log_d(
      "Failed to obtain task from PID. Are you running Austin with the right "
      "privileges?"
    );
    return -1;
  }

  mach_vm_size_t nread;
  kern_return_t kr = mach_vm_read_overwrite(
    task, (mach_vm_address_t) addr, len, (mach_vm_address_t) buf, &nread
  );
  if (kr != KERN_SUCCESS) {
    log_t("copy_memory: mach_vm_read_overwrite returned %d", kr);
    return -1;
  }

  return nread;

  #endif
}

#endif // MEM_H
