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

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../../error.h"
#include "../../hints.h"

#define DELETED_SUFFIX " (deleted)"

// ----------------------------------------------------------------------------
static int
proc_exe_readlink(pid_t pid, char* dest, ssize_t size) {
    char file_name[32];

    sprintf(file_name, "/proc/%d/exe", pid);

    if (readlink(file_name, dest, size) == -1) {
        set_error(IO, "Cannot read symbolic link for executable");
        FAIL; // cppcheck-suppress [resourceLeak]
    }

    // Handle deleted files
    char* suffix = strstr(dest, DELETED_SUFFIX);
    if (isvalid(suffix)) {
        *suffix = '\0';
    }

    SUCCESS;
}
