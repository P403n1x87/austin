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


#if defined(__linux__)
#define _GNU_SOURCE

#include <sys/uio.h>

#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>

#endif

#include <sys/types.h>

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
ssize_t
copy_memory(pid_t, void *, ssize_t, void *);

#endif // MEM_H
