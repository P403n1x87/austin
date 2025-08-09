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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "platform.h"

typedef uint64_t microseconds_t;
typedef uint64_t milliseconds_t;
typedef uint64_t seconds_t;

#define MICROSECONDS_MAX UINT64_MAX

typedef struct {
    microseconds_t t_sampling_interval;
    milliseconds_t timeout;
    pid_t          attach_pid;
    char**         cmd;
    bool           where;
    bool           cpu;
    bool           full;
    bool           memory;
    FILE*          output_file;
    char*          output_filename;
    bool           children;
    seconds_t      exposure;
    bool           pipe;
    bool           gc;
#ifdef NATIVE
    bool kernel;
#endif
} parsed_args_t;

#ifndef ARGPARSE_C
extern parsed_args_t pargs;
#endif

#define ARG_ARGUMENT 0

#define ARG_ERR_EXIT_STATUS       64
#define ARG_STOP_PARSING          1
#define ARG_CONTINUE_PARSING      0
#define ARG_MISSING_OPT_ARG       -1
#define ARG_UNRECOGNISED_LONG_OPT -2
#define ARG_UNRECOGNISED_OPT      -3
#define ARG_INVALID_VALUE         -4
#define ARG_UNEXPECTED_OPT_ARG    -5

int
parse_args(int argc, char** argv);

// TODO: Implement error.
