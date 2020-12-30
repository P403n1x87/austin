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

#include <limits.h>

#include "argparse.h"
#include "austin.h"
#include "hints.h"
#include "platform.h"


#define DEFAULT_SAMPLING_INTERVAL    100
#define DEFAULT_INIT_RETRY_CNT       100

const char SAMPLE_FORMAT_NORMAL[]      = ";%s (%s);L%d";
const char SAMPLE_FORMAT_ALTERNATIVE[] = ";%s (%s:%d)";


// Globals for command line arguments
parsed_args_t pargs = {
  /* t_sampling_interval */ DEFAULT_SAMPLING_INTERVAL,
  /* timeout             */ DEFAULT_INIT_RETRY_CNT * 1000,
  /* attach_pid          */ 0,
  /* exclude_empty       */ 0,
  /* sleepless           */ 0,
  /* format              */ (char *) SAMPLE_FORMAT_NORMAL,
  /* full                */ 0,
  /* memory              */ 0,
  /* output_file         */ NULL,
  /* output_filename     */ NULL,
  /* children            */ 0,
  /* exposure            */ 0,
};

static int exec_arg = 0;


// ---- PRIVATE ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static int
str_to_num(char * str, long * num) {
  char * p_err;

  *num = strtol(str, &p_err, 10);

  return  (p_err == str || *p_err != '\0') ? 1 : 0;
}


/**
 * Parse the interval argument.
 *
 * This acceps s, ms and us as units. The result is in microseconds.
 */
static int
parse_interval(char * str, long * num) {
  char * p_err;

  *num = strtol(str, &p_err, 10);

  if (p_err == str)
    FAIL;

  switch (*p_err) {
  case '\0':
    SUCCESS;
  case 's':
    if (*(p_err+1) != '\0')
      FAIL;
    *num = *num * 1000000;
    break;
  case 'm':
    if (*(p_err+1) != 's' || *(p_err+2) != '\0')
      FAIL;
    *num = *num * 1000;
  case 'u':
    if (*(p_err+1) != 's' || *(p_err+2) != '\0')
      FAIL;
    break;
  default:
    FAIL;
  }

  SUCCESS;
}


/**
 * Parse the timeout argument.
 *
 * This acceps s and ms as units. The result is in milliseconds.
 */
static int
parse_timeout(char * str, long * num) {
  char * p_err;

  *num = strtol(str, &p_err, 10);

  if (p_err == str)
    FAIL;

  switch (*p_err) {
  case '\0':
    SUCCESS;
  case 's':
    if (*(p_err+1) != '\0')
      FAIL;
    *num = *num * 1000;
    break;
  case 'm':
    if (*(p_err+1) != 's' || *(p_err+2) != '\0')
      FAIL;
  default:
    FAIL;
  }

  SUCCESS;
}


// ---- GNU C -----------------------------------------------------------------

#ifdef PL_LINUX                                                      /* LINUX */

#include <argp.h>

const char * argp_program_version = PROGRAM_NAME " " VERSION;

const char * argp_program_bug_address = \
  "<https://github.com/P403n1x87/austin/issues>";

static const char * doc = "Austin -- A frame stack sampler for Python.";

#else

#define ARG_USAGE -1

typedef struct argp_option {
  const char * long_name;
  int          opt;
  const char * has_arg;
  int          _flag;     /* Unused */
  const char * _doc;      /* Unused */
} arg_option;

#endif

static struct argp_option options[] = {
  {
    "interval",     'i', "n_us",        0,
    "Sampling interval in microseconds (default is 100). Accepted units: s, ms, us."
  },
  {
    "timeout",      't', "n_ms",        0,
    "Start up wait time in milliseconds (default is 100). Accepted units: s, ms."
  },
  {
    "alt-format",   'a', NULL,          0,
    "Alternative collapsed stack sample format."
  },
  {
    "exclude-empty",'e', NULL,          0,
    "Do not output samples of threads with no frame stacks."
  },
  {
    "sleepless",    's', NULL,          0,
    "Suppress idle samples."
  },
  {
    "memory",       'm', NULL,          0,
    "Profile memory usage."
  },
  {
    "full",         'f', NULL,          0,
    "Produce the full set of metrics (time +mem -mem)."
  },
  {
    "pid",          'p', "PID",         0,
    "The the ID of the process to which Austin should attach."
  },
  {
    "output",       'o', "FILE",        0,
    "Specify an output file for the collected samples."
  },
  {
    "children",     'C', NULL,          0,
    "Attach to child processes."
  },
  {
    "exposure",     'x', "n_sec",       0,
    "Sample for n_sec seconds only."
  },
  #ifndef PL_LINUX
  {
    "help",         '?', NULL
  },
  {
    "usage",        ARG_USAGE, NULL
  },
  {
    "version",      'V', NULL
  },
  #endif
  {0, 0, 0}
};


#ifdef PL_LINUX

// ----------------------------------------------------------------------------
static int
parse_opt (int key, char *arg, struct argp_state *state)
{
  if (state->argc == 1) {
    state->name = PROGRAM_NAME;  // TODO: Check if there are better ways.
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
    if (
      fail(parse_interval(arg, (long *) &(pargs.t_sampling_interval))) ||
      pargs.t_sampling_interval > LONG_MAX
    )
      argp_error(state, "the sampling interval must be a positive integer");
    break;

  case 't':
    if (
      fail(parse_timeout(arg, (long *) &(pargs.timeout))) ||
      pargs.timeout > LONG_MAX / 1000
    )
      argp_error(state, "timeout must be a positive integer");
    pargs.timeout *= 1000;
    break;

  case 'a':
    pargs.format = (char *) SAMPLE_FORMAT_ALTERNATIVE;
    break;

  case 'e':
    pargs.exclude_empty = 1;
    break;

  case 's':
    pargs.sleepless = 1;
    break;

  case 'm':
    pargs.memory = 1;
    break;

  case 'f':
    pargs.full = 1;
    break;

  case 'p':
    if (str_to_num(arg, &l_pid) == 1 || l_pid <= 0)
      argp_error(state, "invalid PID");
    pargs.attach_pid = (pid_t) l_pid;
    break;

  case 'o':
    pargs.output_file = fopen(arg, "w");
    if (pargs.output_file == NULL) {
      argp_error(state, "Unable to create the given output file");
    }
    pargs.output_filename = arg;
    break;

  case 'C':
    pargs.children = 1;
    break;

  case 'x':
    if (
      str_to_num(arg, (long *) &(pargs.exposure)) == 1 ||
      pargs.exposure > LONG_MAX
    )
      argp_error(state, "the exposure must be a positive integer");
    break;

  case ARGP_KEY_ARG:
  case ARGP_KEY_END:
    if (pargs.attach_pid != 0 && exec_arg != 0)
      argp_error(state, "the -p option is incompatible with the command argument");
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }

  return 0;
}


#else                                                               /* !LINUX */
#include <stdio.h>
#include <string.h>


// Argument callback. Called on every argument parser event.
//
// The first argument is the option character, or 0 for a non-option argument.
// The second argument is either the argument of the option, if one is required,
// or NULL, when the first argument is not null, or the value of the non-option
// argument.
//
// Return 0 to continue parsing the arguments, or otherwise to stop.
typedef int (*arg_callback)(const char opt, const char * arg);


// ----------------------------------------------------------------------------
static arg_option *
_find_long_opt(arg_option * opts, char * opt_name) {
  arg_option * retval = NULL;

  register int i = 0;
  while (retval == NULL && opts[i].opt != 0) {
    if (opts[i].long_name != NULL) {
      char * equal = strchr(opt_name, '=');
      if (equal) *equal = 0;
      if (strcmp(opt_name, opts[i].long_name) == 0) {
        retval = &opts[i];
      }
      if (equal) *equal = '=';
    }

    i++;
  }

  return retval;
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
    char * equal = strchr(argv[argi], '=');
    if (opt->has_arg) {
      if (equal == NULL && (argi >= argc - 1 || argv[argi+1][0] == '-'))
        return ARG_MISSING_OPT_ARG;

      opt_arg = equal ? equal + 1 : argv[argi+1];
    } else if(equal != NULL)
      return ARG_UNEXPECTED_OPT_ARG;

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

  *argi += opt->has_arg && strchr(argv[*argi], '=') == NULL ? 2 : 1;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_handle_opts(arg_option * opts, arg_callback cb, int * argi, int argc, char ** argv) {
  char       * opt_str  = &argv[*argi][1];
  int          n_opts   = strlen(opt_str);
  arg_option * curr_opt = NULL;
  char       * equal    = strchr(argv[*argi], '=');

  for (register int i = 0; i < n_opts; i++) {
    if (opt_str[i] == '=')
      break;
    curr_opt  = _find_opt(opts, opt_str[i]);
    if (curr_opt == NULL)
      return ARG_UNRECOGNISED_OPT;

    if (curr_opt->has_arg && (equal == NULL && i < n_opts - 1))
      return ARG_MISSING_OPT_ARG;
    int cb_res = _handle_opt(curr_opt, cb, *argi, argc, argv);
    if (cb_res)
      return cb_res;
  }

  *argi += curr_opt->has_arg && equal == NULL ? 2 : 1;

  return 0;
}


static const char * help_msg = \
"Usage: austin [OPTION...] command [ARG...]\n"
"Austin -- A frame stack sampler for Python.\n"
"\n"
"  -a, --alt-format           Alternative collapsed stack sample format.\n"
"  -C, --children             Attach to child processes.\n"
"  -e, --exclude-empty        Do not output samples of threads with no frame\n"
"                             stacks.\n"
"  -f, --full                 Produce the full set of metrics (time +mem -mem).\n"
"  -i, --interval=n_us        Sampling interval in microseconds (default is\n"
"                             100). Accepted units: s, ms, us.\n"
"  -m, --memory               Profile memory usage.\n"
"  -o, --output=FILE          Specify an output file for the collected samples.\n"
"  -p, --pid=PID              The the ID of the process to which Austin should\n"
"                             attach.\n"
"  -s, --sleepless            Suppress idle samples.\n"
"  -t, --timeout=n_ms         Start up wait time in milliseconds (default is\n"
"                             100). Accepted units: s, ms.\n"
"  -x, --exposure=n_sec       Sample for n_sec seconds only.\n"
"  -?, --help                 Give this help list\n"
"      --usage                Give a short usage message\n"
"  -V, --version              Print program version\n"
"\n"
"Mandatory or optional arguments to long options are also mandatory or optional\n"
"for any corresponding short options.\n"
"\n"
"Report bugs to <https://github.com/P403n1x87/austin/issues>.\n";

static const char * usage_msg = \
"Usage: austin [-aCefms?V] [-i n_us] [-o FILE] [-p PID] [-t n_ms] [-x n_sec]\n"
"            [--alt-format] [--children] [--exclude-empty] [--full]\n"
"            [--interval=n_us] [--memory] [--output=FILE] [--pid=PID]\n"
"            [--sleepless] [--timeout=n_ms] [--exposure=n_sec] [--help]\n"
"            [--usage] [--version] command [ARG...]\n";


static void
arg_error(const char * message) {
  fputs(PROGRAM_NAME ": ", stderr);
  fputs(message, stderr);
  fputc('\n', stderr);
  fputs("Try `austin --help' or `austin --usage' for more information.\n", stderr);
  exit(ARG_INVALID_VALUE);
}


// ----------------------------------------------------------------------------
// Return 0 if all the arguments have been parsed. If interrupted, returns the
// number of arguments consumed so far. Otherwise return an error code.
static int
arg_parse(arg_option * opts, arg_callback cb, int argc, char ** argv) {
  int a      = 1;
  int cb_res = 0;

  if (argc <= 1) {
    puts(usage_msg);
    exit(0);
  }

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


// ----------------------------------------------------------------------------
static int
cb(const char opt, const char * arg) {
  switch (opt) {
  case 'i':
    if (
      fail(parse_interval((char *) arg, (long *) &(pargs.t_sampling_interval))) ||
      pargs.t_sampling_interval > LONG_MAX
    ) {
      arg_error("the sampling interval must be a positive integer");
    }
    break;

  case 't':
    if (
      fail(parse_timeout((char *) arg, (long *) &(pargs.timeout))) ||
      pargs.timeout > LONG_MAX / 1000
    ) {
      arg_error("the timeout must be a positive integer");
    }
    pargs.timeout *= 1000;
    break;

  case 'a':
    pargs.format = (char *) SAMPLE_FORMAT_ALTERNATIVE;
    break;

  case 'e':
    pargs.exclude_empty = 1;
    break;

  case 's':
    pargs.sleepless = 1;
    break;

  case 'm':
    pargs.memory = 1;
    break;

  case 'f':
    pargs.full = 1;
    break;

  case 'p':
    if (
      str_to_num((char *) arg, (long *) &pargs.attach_pid) == 1 ||
      pargs.attach_pid <= 0
    ) {
      arg_error("invalid PID");
    }
    break;

  case 'o':
    pargs.output_file = fopen(arg, "w");
    if (pargs.output_file == NULL) {
      puts("Unable to create the given output file.");
      return ARG_INVALID_VALUE;
    }
    pargs.output_filename = (char *) arg;
    break;

  case 'C':
    pargs.children = 1;
    break;

  case 'x':
    if (
      str_to_num((char *) arg, (long *) &(pargs.exposure)) == 1 ||
      pargs.exposure > LONG_MAX
    ) {
      arg_error("the exposure must be a positive integer");
    }
    break;

  case '?':
    puts(help_msg);
    exit(0);

  case 'V':
    puts(PROGRAM_NAME " " VERSION);
    exit(0);

  case ARG_USAGE:
    puts(usage_msg);
    exit(0);

  case ARG_ARGUMENT:
    return ARG_STOP_PARSING;

  default:
    puts(usage_msg);
    exit(ARG_UNRECOGNISED_OPT);
  }

  return ARG_CONTINUE_PARSING;
}

#endif


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
parse_args(int argc, char ** argv) {
  #ifdef PL_LINUX
  struct argp args = {options, parse_opt, "command [ARG...]", doc};
  argp_parse(&args, argc, argv, 0, 0, 0);

  #else
  exec_arg = arg_parse(options, cb, argc, argv) - 1;
  #endif

  return exec_arg;
}
