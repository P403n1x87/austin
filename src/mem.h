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

#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "hints.h"
#include "platform.h"

#if defined PL_LINUX
#include <sys/uio.h>
#include <unistd.h>
ssize_t
process_vm_readv(
    pid_t, const struct iovec*, unsigned long liovcnt, const struct iovec* remote_iov, unsigned long riovcnt,
    unsigned long flags
);

#elif defined(PL_WIN)
#include <windows.h>
__declspec(dllimport) extern BOOL GetPhysicallyInstalledSystemMemory(PULONGLONG);

#elif defined(PL_MACOS)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/machine/kern_return.h>
#include <sys/sysctl.h>
#include <sys/types.h>

#endif

#include "error.h"
#include "logging.h"

/**
 * Copy a data structure from the given remote address structure.
 * @param  pref the process reference
 * @param  addr the remote address
 * @param  dt   the data structure as a local variable
 * @return      zero on success, otherwise non-zero.
 */
#define copy_remote(pref, addr, dt) copy_memory(pref, addr, sizeof(dt), &dt)

/**
 * Copy a data structure from the given remote address structure.
 * @param  raddr the remote address
 * @param  dt    the data structure as a local variable
 * @return       zero on success, otherwise non-zero.
 */
#define copy_remote_v(pref, addr, dt, n) copy_memory(pref, addr, n, &dt)

/**
 * Same as copy_remote, but with explicit arguments instead of a pointer to
 * a remote address structure
 * @param  pref the process reference
 * @param  addr the remote address
 * @param  dt   the data structure as a local variable.
 * @return      zero on success, otherwise non-zero.
 */
#define copy_datatype(pref, addr, dt) copy_memory(pref, addr, sizeof(dt), &dt)

/**
 * Same as copy_remote, but for versioned Python data structures.
 * @param  pref     the process reference
 * @param  addr     the remote address
 * @param  py_type  the versioned Python type (e.g. py_runtime).
 * @param  dest     the destination variable.
 * @return          zero on success, otherwise non-zero.
 */
#define copy_py(pref, addr, py_type, dest) copy_memory(pref, addr, py_v->py_type.size, &dest)

/**
 * Copy a field from a versioned Python data structure.
 * @param  pref   the process reference
 * @param  type   the versioned Python type (e.g. runtime).
 * @param  field  the field name (e.g. interp_head).
 * @param  raddr  the remote address of the versioned Python data structure.
 * @param  dst    the destination variable.
 * @return        zero on success, otherwise non-zero.
 */
#define copy_field_v(pref, type, field, raddr, dst)                         \
    copy_memory(pref, raddr + py_v->py_##type.o_##field, sizeof(dst), &dst)

typedef void* raddr_t;

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
copy_memory(proc_ref_t proc_ref, raddr_t restrict addr, ssize_t len, void* restrict buf) {
    ssize_t result = -1;

#if defined(PL_LINUX) /* LINUX */
    struct iovec local[1];
    struct iovec remote[1];

    local[0].iov_base  = buf;
    local[0].iov_len   = len;
    remote[0].iov_base = addr;
    remote[0].iov_len  = len;

    result = process_vm_readv(proc_ref, local, 1, remote, 1, 0);
    if (result == -1) {
        switch (errno) {
        case ESRCH:
            set_error(OS, "No such process");
            break;
        case EPERM:
            set_error(PERM, "Remote memory read access denied");
            break;
        default:
            set_error(MEMCOPY, "Cannot copy remote memory");
        }
    }

#elif defined(PL_WIN) /* WIN */
    size_t n;
    result = ReadProcessMemory(proc_ref, addr, buf, len, &n) ? n : -1;
    if (result == -1) {
        switch (GetLastError()) {
        case ERROR_ACCESS_DENIED:
            set_error(PERM, "Remote memory read access denied");
            break;
        case ERROR_INVALID_HANDLE:
            set_error(OS, "No such process");
            break;
        default:
            set_error(MEMCOPY, "Cannot copy remote memory");
        }
    }

#elif defined(PL_MACOS) /* MAC */
    kern_return_t kr = mach_vm_read_overwrite(
        proc_ref, (mach_vm_address_t)addr, len, (mach_vm_address_t)buf, (mach_vm_size_t*)&result
    );
    if (unlikely(kr != KERN_SUCCESS)) {
        // If we got to the point of calling this function on macOS then we must
        // have permissions to call task_for_pid successfully. This also means that
        // the PID that was used must have been valid. Therefore this call can only
        // fail if the process no longer exists. However, if the return value is
        // MACH_SEND_INVALID_DEST, we probably tried an invalid memory area.
        switch (kr) {
        case KERN_PROTECTION_FAILURE:
            set_error(PERM, "Protection failure on remote memory read");
            break;
        case KERN_INVALID_ARGUMENT:
            set_error(OS, "No such process");
            break;
        default:
            set_error(MEMCOPY, "Could not copy remote memory");
        }
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
#if defined PL_LINUX /* LINUX */
    size_t pagesize = getpagesize() >> 10;
    return sysconf(_SC_PHYS_PAGES) * pagesize;

#elif defined PL_MACOS /* MAC */
    int     mib[] = {CTL_HW, HW_PHYSMEM};
    int64_t size;
    size_t  length = sizeof(size);

    return success(sysctl(mib, 2, &size, &length, NULL, 0)) ? size >> 10 : 0;

#elif defined PL_WIN /* WIN */
    ULONGLONG size;
    return GetPhysicallyInstalledSystemMemory(&size) ? size : 0;

#endif

    return 0;
}

struct vm_map {
    char*   path;
    ssize_t file_size;
    raddr_t base;
    size_t  size;
    raddr_t bss_base;
    size_t  bss_size;
    bool    has_symbols;
};

enum {
    MAP_BIN,
    MAP_LIBSYM,
    MAP_LIBNEEDLE,
    MAP_COUNT,
};

struct proc_desc {
    char          exe_path[1024];
    struct vm_map maps[MAP_COUNT];
};
