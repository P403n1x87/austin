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
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#include "../hints.h"
#include "../py_thread.h"
#include "../resources.h"

// ----------------------------------------------------------------------------
bool
py_thread__is_idle(py_thread_t* self) {
#ifdef NATIVE
    size_t index  = self->tid >> 3;
    int    offset = self->tid & 7;

    return _tids_idle[index] & (1 << offset);
#else
    char file_name[64];
    char buffer[2048] = "";

    sprintf(file_name, "/proc/%d/task/%" PRIuPTR "/stat", self->proc->pid, self->tid);

    cu_fd fd = open(file_name, O_RDONLY);
    if (fd == -1) { // GCOV_EXCL_START
        set_error(IO, "Cannot open thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    if (read(fd, buffer, 2047) == 0) { // GCOV_EXCL_START
        set_error(IO, "Cannot read thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    char* p = strchr(buffer, ')'); // GCOV_EXCL_START
    if (!isvalid(p)) {
        set_error(OS, "Invalid thread stat file");
        FAIL_BOOL;
    } // GCOV_EXCL_STOP

    p += 2;
    if (*p == ' ')
        p++; // GCOV_EXCL_LINE

    return (*p != 'R');
#endif
}
