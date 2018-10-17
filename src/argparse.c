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

#define ARGPARSE_C

#include <stdlib.h>

#include "argparse.h"


#define DEFAULT_SAMPLING_INTERVAL    100

const char SAMPLE_FORMAT_NORMAL[]      = ";%s (%s);L%d";
const char SAMPLE_FORMAT_ALTERNATIVE[] = ";%s (%s:%d)";


// Globals for command line arguments
ctime_t t_sampling_interval = DEFAULT_SAMPLING_INTERVAL;
pid_t   attach_pid          = 0;
int     exclude_empty       = 0;
int     sleepless           = 0;
char *  format              = (char *) SAMPLE_FORMAT_NORMAL;

static int exec_arg = 0;


// ---- PRIVATE ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static int
strtonum(char * str, long * num) {
  char * p_err;

  *num = strtol(str, &p_err, 10);

  return  (p_err == str || *p_err != 0) ? 1 : 0;
}


// ---- GNU C -----------------------------------------------------------------

#if defined(__linux__)

#include <argp.h>

#include "austin.h"

const char * argp_program_version = PROGRAM_NAME " " VERSION;

const char * argp_program_bug_address = \
  "<https://github.com/P403n1x87/austin/issues>";

static const char * doc = "Austin -- A frame stack sampler for Python.";

static struct argp_option options[] = {
  {
    "interval",     'i', "n_usec",      0,
    "Sampling interval (default is 500 usec)."
  },
  {
    "alt-format",   'a', NULL,          0,
    "alternative collapsed stack sample format."
  },
  {
    "exclude-empty",'e', NULL,          0,
    "do not output samples of threads with no frame stacks."
  },
  {
    "sleepless",    's', NULL,          0,
    "suppress idle samples."
  },
  {
    "pid",          'p', "PID",         0,
    "The the ID of the process to which Austin should attach."
  },
  {0}
};


// ----------------------------------------------------------------------------
static int
parse_opt (int key, char *arg, struct argp_state *state)
{
  if (state->argc == 1) {
    argp_state_help(state, stdout, ARGP_HELP_USAGE);
    exit(0);
  }

  // Consume all the remaining arguments if the next one is not an option so
  // that they can be passed to the command to execute
  if ((state->next == 0 && state->argv[1][0] != '-')
  ||  (state->next > 0 && state->next < state->argc && state->argv[state->next][0] != '-')
  ) {
    exec_arg = state->next == 0 ? 1 : state->next;
    state->next = state->argc;
  }

  long l_pid;
  switch(key) {
  case 'i':
    if (strtonum(arg, (long *) &t_sampling_interval) == 1 || t_sampling_interval < 0)
      argp_error(state, "the sampling interval must be a positive integer");
    break;

  case 'a':
    format = (char *) SAMPLE_FORMAT_ALTERNATIVE;
    break;

  case 'e':
    exclude_empty = 1;
    break;

  case 's':
    sleepless = 1;
    break;

  case 'p':
    if (strtonum(arg, &l_pid) == 1 || l_pid <= 0)
      argp_error(state, "invalid PID.");
    attach_pid = (pid_t) l_pid;
    break;

  case ARGP_KEY_ARG:
  case ARGP_KEY_END:
    if (attach_pid != 0 && exec_arg != 0)
      argp_error(state, "the -p option is incompatible with the command argument.");
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }

  return 0;
}

#else


// ---- OTHER C ---------------------------------------------------------------

#include <string.h>

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


// ----------------------------------------------------------------------------
static int
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


static const char * help_msg = \
"Usage: austin [OPTION...] command [ARG...]\n"
"Austin -- A frame stack sampler for Python.\n"
"\n"
"  -a, --alt-format           alternative collapsed stack sample format.\n"
"  -e, --exclude-empty        do not output samples of threads with no frame\n"
"                             stacks.\n"
"  -i, --interval=n_usec      Sampling interval (default is 500 usec).\n"
"  -p, --pid=PID              The the ID of the process to which Austin should\n"
"                             attach.\n"
"  -s, --sleepless            suppress idle samples.\n"
"  -?, --help                 Give this help list\n"
"      --usage                Give a short usage message\n"
"  -V, --version              Print program version\n"
"\n"
"Mandatory or optional arguments to long options are also mandatory or optional\n"
"for any corresponding short options.\n"
"\n"
"Report bugs to <https://github.com/P403n1x87/austin/issues>.\n";

static const char * usage_msg = \
"Usage: austin [-aes?V] [-i n_usec] [-p PID] [--alt-format] [--exclude-empty]\n"
"            [--interval=n_usec] [--pid=PID] [--sleepless] [--help] [--usage]\n"
"            [--version] command [ARG...]\n";

static arg_option opts[] = {
  {
    "interval",     'i', 1
  },
  {
    "alt-format",   'a', 0
  },
  {
    "exclude-empty",'e', 0
  },
  {
    "sleepless",    's', 0
  },
  {
    "pid",          'p', 1
  },
  {
    "help",         '?', 0
  },
  {
    "usage",        -1,  0
  },
  {
    "version",      'V', 0
  },
  {0, 0, 0}
};

int cb(const char opt, const char * arg) {
  pid_t l_pid;

  switch (opt) {
  case 'i':
    if (strtonum((char *) arg, (long *) &t_sampling_interval) == 1 || t_sampling_interval < 0) {
      printf(usage_msg);
      return ARG_INVALID_VALUE;
    }
    break;

  case 'a':
    format = (char *) SAMPLE_FORMAT_ALTERNATIVE;
    break;

  case 'e':
    exclude_empty = 1;
    break;

  case 's':
    sleepless = 1;
    break;

  case 'p':
    if (strtonum((char *) arg, (long *) &l_pid) == 1 || l_pid <= 0) {
      printf(usage_msg);
      return ARG_INVALID_VALUE;
    }
    attach_pid = (pid_t) l_pid;
    break;

  case '?':
    printf(help_msg);
    exit(0);

  case 'V':
    printf(PROGRAM_NAME " " VERSION);
    exit(0);

  case -1:
    printf(usage_msg);
    exit(0);

  default:
    printf(usage_msg);
    return ARG_STOP_PARSING;
  }

  return ARG_CONTINUE_PARSING;
}


#endif


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
parse_args(int argc, char ** argv) {
  #if defined(__linux__)
  struct argp args = {options, parse_opt, "command [ARG...]", doc};
  argp_parse(&args, argc, argv, 0, 0, 0);

  #else
  exec_arg = arg_parse(opts, cb, argc, argv) - 1;
  #endif

  return exec_arg;
}
