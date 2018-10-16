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

#if defined(_WIN32) || defined(_WIN64)

#include <string.h>

#include "argparse.h"


// ---- PRIVATE ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static arg_option *
_find_long_opt(arg_option * opts, char * opt_name) {
  register int i = 0;

  while (opts[i].opt != 0) {
    if (opts[i].long_name != NULL) {
      if (strcmp(opt_name, opts[i].long_name) == 0)
        return &opts[i];
    }

    i++;
  }

  return NULL;
}


// ----------------------------------------------------------------------------
static arg_option *
_find_opt(arg_option * opts, char opt) {
  register int i = 0;

  while (opts[i].opt != 0) {
    if (opts[i].opt == opt)
      return &opts[i];

    i++;
  }

  return NULL;
}


// ----------------------------------------------------------------------------
static int
_handle_opt(arg_option * opt, arg_callback cb, int argi, int argc, char ** argv) {
  char * opt_arg = NULL;

  if (opt) {
    if (opt->has_arg) {
      if (argi >= argc - 1 || argv[argi+1][0] == '-')
        return ARG_MISSING_OPT_ARG;

      opt_arg = argv[++argi];
    }

    return cb(opt->opt, opt_arg);
  }

  return ARG_UNRECOGNISED_LONG_OPT;
}


// ----------------------------------------------------------------------------
static int
_handle_long_opt(arg_option * opts, arg_callback cb, int * argi, int argc, char ** argv) {
  arg_option * opt = _find_long_opt(opts, &argv[*argi][2]);
  int cb_res = _handle_opt(opt, cb, *argi, argc, argv);
  if (cb_res)
    return cb_res;

  *argi += opt->has_arg ? 2 : 1;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_handle_opts(arg_option * opts, arg_callback cb, int * argi, int argc, char ** argv) {
  char       * opt_str  = &argv[*argi][1];
  int          n_opts   = strlen(opt_str);
  arg_option * curr_opt = NULL;
  for (register int i = 0; i < n_opts; i++) {
    curr_opt  = _find_opt(opts, opt_str[i]);
    if (curr_opt == NULL)
      return ARG_UNRECOGNISED_OPT;

    if (curr_opt->has_arg && i < n_opts - 1)
      return ARG_MISSING_OPT_ARG;
    int cb_res = _handle_opt(curr_opt, cb, *argi, argc, argv);
    if (cb_res)
      return cb_res;
  }

  *argi += curr_opt->has_arg ? 2 : 1;

  return 0;
}


// ---- PUBLIC ----------------------------------------------------------------

int
arg_parse(arg_option * opts, arg_callback cb, int argc, char ** argv) {
  int a      = 1;
  int cb_res = 0;

  while (a < argc) {
    if (argv[a][0] == '-') {
      if (argv[a][1] == '-') {
        // Long option
        cb_res = _handle_long_opt(opts, cb, &a, argc, argv);
      }
      else {
        // Simple option
        cb_res = _handle_opts(opts, cb, &a, argc, argv);
      }
    }
    else {
      // Argument
      cb_res = cb(0, argv[a++]);
    }

    if (cb_res)
      return cb_res < 0 ? cb_res : a;
  }

  return 0;
}

#endif
