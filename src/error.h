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

#ifndef ERROR_H
#define ERROR_H

#include "logging.h"


// generic messages
#define EOK                   0
#define EMMAP                 1

// py_code_t
#define ECODE                 ((1 << 3) + 0)
#define ECODEFMT              ((1 << 3) + 1)
#define ECODECMPT             ((1 << 3) + 2)
#define ECODEBYTES            ((1 << 3) + 3)
#define ECODENOFNAME          ((1 << 3) + 4)
#define ECODENONAME           ((1 << 3) + 5)
#define ECODENOLINENO         ((1 << 3) + 6)
#define ECODEUNICODE          ((1 << 3) + 7)

// py_frame_t
#define EFRAME                ((2 << 3) + 0)
#define EFRAMENOCODE          ((2 << 3) + 1)
#define EFRAMEINV             ((2 << 3) + 2)

// py_thread_t
#define ETHREAD               ((3 << 3) + 0)
#define ETHREADNOFRAME        ((3 << 3) + 1)
#define ETHREADINV            ((3 << 3) + 2)

// py_proc_t
#define EPROC                 ((4 << 3) + 0)
#define EPROCFORK             ((4 << 3) + 1)
#define EPROCVM               ((4 << 3) + 2)
#define EPROCISTIMEOUT        ((4 << 3) + 3)
#define EPROCATTACH           ((4 << 3) + 4)
#define EPROCPERM             ((4 << 3) + 5)
#define EPROCNPID             ((4 << 3) + 6)


#ifdef ERROR_C
__thread int error;
#else
extern __thread int error;
#endif // ERROR_C


/**
 * Log the last error
 */
#define log_error() ( is_fatal(error) ? log_f(error_get_msg(error)) : log_e(error_get_msg(error)) )

/**
 * Get the message of the last error.
 *
 * @return a pointer to the message as const char *.
 */
#define get_error() error_get_msg(error)


typedef int error_t;

/**
 * Get the message of the give message number.
 *
 * @param  error_t  the error number
 *
 * @return a pointer to the message as const char *.
 */
const char *
error_get_msg(error_t);


/**
 * Determine if the given error is fatal or not.
 *
 * @param  error_t  the error number
 *
 * @return 1 if the error is fatal, 0 otherwise.
 */
const int
is_fatal(error_t);


void
check_not_null(void *);

#endif // ERROR_H
