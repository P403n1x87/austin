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

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "hints.h"
#include "platform.h"

#if defined PL_LINUX
static size_t max_pid = 0;
#endif

// ----------------------------------------------------------------------------
size_t
pid_max() {
#if defined PL_LINUX    /* LINUX */
    if (max_pid)        // GCOV_EXCL_LINE
        return max_pid; // GCOV_EXCL_LINE

    FILE* pid_max_file = fopen("/proc/sys/kernel/pid_max", "rb");
    if (!isvalid(pid_max_file)) // GCOV_EXCL_LINE
        return 0;               // GCOV_EXCL_LINE

    bool has_pid_max = (fscanf(pid_max_file, "%zu", &max_pid) == 1);
    fclose(pid_max_file);
    if (!has_pid_max) // GCOV_EXCL_LINE
        return 0;     // GCOV_EXCL_LINE

    return max_pid;

#elif defined PL_MACOS /* MACOS */
    return PID_MAX;

#elif defined PL_WIN /* WIN */
    return (1 << 22); // 4M.  WARNING: This could potentially be violated!

#endif

    return 0;
}

// ----------------------------------------------------------------------------
size_t
get_page_size() {
#if defined PL_UNIX /* UNIX */
    return sysconf(_SC_PAGESIZE);
#elif defined PL_WIN /* WIN */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#endif
    return 0;
}

// ----------------------------------------------------------------------------
#if defined PL_WIN
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#endif

bool
is_tty(FILE* output) {
    return isatty(fileno(output));
}
