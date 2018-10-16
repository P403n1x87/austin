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

#ifndef ARGPARSE_H
#define ARGPARSE_H

#include <stdlib.h>


#define ARG_STOP_PARSING               1
#define ARG_CONTINUE_PARSING           0
#define ARG_MISSING_OPT_ARG           -1
#define ARG_UNRECOGNISED_LONG_OPT     -2
#define ARG_UNRECOGNISED_OPT          -3
#define ARG_INVALID_VALUE             -4


typedef struct {
  const char *        long_name;
  const char          opt;
  int                 has_arg;
} arg_option;


// Argument callback. Called on every argument parser event.
//
// The first argument is the option character, or 0 for a non-option argument.
// The second argument is either the argument of the option, if one is required,
// or NULL, when the first argument is not null, or the value of the non-option
// argument.
//
// Return 0 to continue parsing the arguments, or otherwise to stop.
typedef int (*arg_callback)(const char opt, const char * arg);


// Return 0 if all the arguments have been parsed. If interrupted, returns the
// number of arguments consumed so far. Otherwise return an error code.
int arg_parse(arg_option * opts, arg_callback cb, int argc, char ** argv);

// TODO: Implement error.

#endif
