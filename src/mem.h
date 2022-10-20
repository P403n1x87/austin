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

#include "hints.h"
#include "platform.h"

#if defined PL_LINUX
  #include <sys/uio.h>
  #include <unistd.h>
  ssize_t process_vm_readv(
    pid_t, const struct iovec *, unsigned long liovcnt,
    const struct iovec *remote_iov, unsigned long riovcnt, unsigned long flags
  );

#elif defined(PL_WIN)
  #include <windows.h>
  __declspec( dllimport ) extern BOOL GetPhysicallyInstalledSystemMemory(PULONGLONG);

#elif defined(PL_MACOS)
  #include <mach/mach.h>
  #include <mach/mach_vm.h>
  #include <mach/machine/kern_return.h>
  #include <sys/types.h>
  #include <sys/sysctl.h>

#endif

#include "error.h"
#include "logging.h"

#define OUT_OF_BOUND                  -1


/**
 * Copy a data structure from the given remote address structure.
 * @param  raddr the remote address
 * @param  dt    the data structure as a local variable
 * @return       zero on success, otherwise non-zero.
 */
#define copy_from_raddr(raddr, dt) copy_memory((raddr)->pref, (raddr)->addr, sizeof(dt), &dt)


/**
 * Copy a data structure from the given remote address structure.
 * @param  raddr the remote address
 * @param  dt    the data structure as a local variable
 * @return       zero on success, otherwise non-zero.
 */
#define copy_from_raddr_v(raddr, dt, n) copy_memory(raddr->pref, raddr->addr, n, &dt)


/**
 * Same as copy_from_raddr, but with explicit arguments instead of a pointer to
 * a remote address structure
 * @param  pref the process reference
 * @param  addr the remote address
 * @param  dt   the data structure as a local variable.
 * @return      zero on success, otherwise non-zero.
 */
#define copy_datatype(pref, addr, dt) copy_memory(pref, addr, sizeof(dt), &dt)


/**
 * Same as copy_from_raddr, but for versioned Python data structures.
 * @param  pref     the process reference
 * @param  addr     the remote address
 * @param  py_type  the versioned Python type (e.g. py_runtime).
 * @param  dest     the destination variable.
 * @return          zero on success, otherwise non-zero.
 */
#define copy_py(pref, addr, py_type, dest) copy_memory(pref, addr, py_v->py_type.size, &dest)


// Whilst the PID is generally used to identify processes across platforms,
// operations can only be performed on other process references, like a Win32
// HANDLE or a OSX mach_port_t. We use this structure to abstract the process
// reference to identify a remote address location in a platoform-independent
// way.
typedef struct {
  proc_ref_t   pref;  // Process reference
  void       * addr;  // Virtual memory address within the process
} raddr_t;


/**
 * Copy a chunk of memory from a portion of the virtual memory of another
 * process.
 * @param proc_ref_t  the process reference (platform-dependent)
 * @param void *      the remote address
 * @param ssize_t     the number of bytes to read
 * @param void *      the destination buffer, expected to be at least as large
 *                    as the number of bytes to read.
 * 
 * @return  zero on success, otherwise non-zero.
 */
static inline int
copy_memory(proc_ref_t proc_ref, void * addr, ssize_t len, void * buf) {
  ssize_t result = -1;

  #if defined(PL_LINUX)                                              /* LINUX */
  struct iovec local[1];
  struct iovec remote[1];

  local[0].iov_base = buf;
  local[0].iov_len = len;
  remote[0].iov_base = addr;
  remote[0].iov_len = len;

  result = process_vm_readv(proc_ref, local, 1, remote, 1, 0);
  if (result == -1) {
    switch (errno) {
    case ESRCH:
      set_error(EPROCNPID);
      break;
    case EPERM:
      set_error(EPROCPERM);
      break;
    default:
      set_error(EMEMCOPY);
    }
  }

  #elif defined(PL_WIN)                                                /* WIN */
  size_t n;
  result = ReadProcessMemory(proc_ref, addr, buf, len, &n) ? n : -1;
  if (result == -1) {
    switch(GetLastError()) {
    case ERROR_ACCESS_DENIED:
      set_error(EPROCPERM);
      break;
    case ERROR_INVALID_HANDLE:
      set_error(EPROCNPID);
      break;
    default:
      set_error(EMEMCOPY);
    }
  }

  #elif defined(PL_MACOS)                                              /* MAC */
  kern_return_t kr = mach_vm_read_overwrite(
    proc_ref,
    (mach_vm_address_t) addr,
    len,
    (mach_vm_address_t) buf,
    (mach_vm_size_t *) &result
  );
  if (unlikely(kr != KERN_SUCCESS)) {
    // If we got to the point of calling this function on macOS then we must
    // have permissions to call task_for_pid successfully. This also means that
    // the PID that was used must have been valid. Therefore this call can only
    // fail if the process no longer exists. However, if the return value is
    // MACH_SEND_INVALID_DEST, we probably tried an invalid memory area.
    if (kr != MACH_SEND_INVALID_DEST)
      set_error(EPROCNPID);
    FAIL;
  }

  #endif

  return result != len;
}

/**
 * Return the total physical memory installed on the system, in KB.
 * @return  the total physical memory installed on the system, in KB.
 */
static inline size_t
get_total_memory(void) {
  #if defined PL_LINUX                                               /* LINUX */
  size_t pagesize = getpagesize() >> 10;
  return sysconf (_SC_PHYS_PAGES) * pagesize;

  #elif defined PL_MACOS                                               /* MAC */
  int mib [] = { CTL_HW, HW_PHYSMEM };
  int64_t size;
  size_t length = sizeof(size);

  return success(sysctl(mib, 2, &size, &length, NULL, 0)) ? size >> 10 : 0;

  #elif defined PL_WIN                                                 /* WIN */
  ULONGLONG size;
  return GetPhysicallyInstalledSystemMemory(&size) == TRUE ? size : 0;

  #endif

  return 0;
}

#endif // MEM_H
