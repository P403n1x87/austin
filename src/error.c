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

#define ERROR_C

#include <stdlib.h>

#include "error.h"


#define MAXERROR              (5 << 3)

const char * _error_msg_tab[MAXERROR] = {
  // generic error messages
  "No error",
  "Unable to open memory maps file.",
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,

  // py_code_t
  "Failed to retrieve PyCodeObject",
  "Encountered unsupported string format",
  "Not a compact unicode object",
  "Failed to retrieve PyBytesObject",
  "Unable to get filename from code object",
  "Unable to get function name from code object",
  "Unable to get line number from code object",
  "Failed to retrieve PyUnicodeObject",

  // py_frame_t
  "Failed to create frame object",
  "Failed to get code object for frame",
  "Invalid frame",
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,

  // py_thread_t
  "Failed to create thread object",
  "Failed to get top frame for thread",
  "Invalid thread",
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,

  // py_proc_t
  "Failed to retrieve interpreter state",
  "Failed to fork process",
  "Failed to load memory maps",
  "Interpreter state search timed out",
  "Failed to attach to running process",
  "Permission denied. Try with elevated privileges.",
  "No such process.",
  NULL,
};


const int _fatal_error_tab[MAXERROR] = {
  // generic error messages
  0,
  1,
  0,
  0,
  0,
  0,
  0,
  0,

  // py_code_t
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,

  // py_frame_t
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,

  // py_thread_t
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,

  // py_proc_t
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
};


const char *
error_get_msg(error_t n) {
  if (n >= MAXERROR)
    return "<Unknown error>";

  return _error_msg_tab[n];
}


const int
is_fatal(error_t n) {
  if (n >= MAXERROR)
    return 0;

  return _fatal_error_tab[n];
}
