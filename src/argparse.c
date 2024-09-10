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

#include "platform.h"

#ifdef PL_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include <limits.h>

#include "argparse.h"
#include "austin.h"
#include "hints.h"
#include "platform.h"

#if defined PL_LINUX && !defined __MUSL__
#define GNU_ARGP
#endif

#ifdef NATIVE
#define DEFAULT_SAMPLING_INTERVAL  10000  // reduces impact on tracee
#else
#define DEFAULT_SAMPLING_INTERVAL    100
#endif
#define DEFAULT_INIT_TIMEOUT_MS     1000  // 1 second
#define DEFAULT_HEAP_SIZE              0

const char SAMPLE_FORMAT_NORMAL[]      = ";%s:%s:%d";
const char SAMPLE_FORMAT_WHERE[]       = "    \033[33;1m%2$s\033[0m (\033[36;1m%1$s\033[0m:\033[32;1m%3$d\033[0m)\n";
#ifdef NATIVE
const char SAMPLE_FORMAT_WHERE_NATIVE[]= "    \033[38;5;246m%2$s\033[0m (\033[38;5;248;1m%1$s\033[0m:\033[38;5;246m%3$d\033[0m)\n";
const char SAMPLE_FORMAT_KERNEL[]      = ";kernel:%s:0";
const char SAMPLE_FORMAT_WHERE_KERNEL[]= "    \033[38;5;159m%s\033[0m 🐧\n";
#endif
#if defined PL_WIN
const char HEAD_FORMAT_DEFAULT[]       = "P%I64d;T%I64x:%I64x";
const char HEAD_FORMAT_WHERE[]         = "\n\n%4$s%5$s Process \033[35;1m%1$I64d\033[0m 🧵 Thread \033[34;1m%2$I64d:%3$I64d\033[0m\n\n";
#else
const char HEAD_FORMAT_DEFAULT[]       = "P%d;T%ld:%ld";
const char HEAD_FORMAT_WHERE[]         = "\n\n%4$s%5$s Process \033[35;1m%1$d\033[0m 🧵 Thread \033[34;1m%2$ld:%3$ld\033[0m\n\n";
#endif


// Globals for command line arguments
parsed_args_t pargs = {
  /* t_sampling_interval */ DEFAULT_SAMPLING_INTERVAL,
  /* timeout             */ DEFAULT_INIT_TIMEOUT_MS * 1000,
  /* attach_pid          */ 0,
  /* where               */ 0,
  /* sleepless           */ 0,
  /* format              */ (char *) SAMPLE_FORMAT_NORMAL,
  #ifdef NATIVE
  /* native_format       */ (char *) SAMPLE_FORMAT_NORMAL,
  /* kernel_format       */ (char *) SAMPLE_FORMAT_KERNEL,
  #endif
  /* head_format         */ (char *) HEAD_FORMAT_DEFAULT,
  /* full                */ 0,
  /* memory              */ 0,
  /* binary              */ 0,
  /* output_file         */ NULL,
  /* output_filename     */ NULL,
  /* children            */ 0,
  /* exposure            */ 0,
  /* pipe                */ 0,
  /* gc                  */ 0,
  /* heap                */ DEFAULT_HEAP_SIZE,
  #ifdef NATIVE
  /* kernel              */ 0,
  #endif
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
 * This accepts s, ms and us as units. The result is in microseconds.
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
 * This accepts s and ms as units. The result is in milliseconds.
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

#ifdef GNU_ARGP                                                      /* LINUX */

#include <argp.h>

const char * argp_program_version = PROGRAM_NAME " " VERSION;

const char * argp_program_bug_address = \
  "<https://github.com/P403n1x87/austin/issues>";

static const char * doc = \
"Austin is a frame stack sampler for CPython that is used to extract profiling "
"data out of a running Python process (and all its children, if required) "
"that requires no instrumentation and has practically no impact on the tracee.";

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
    "sleepless",    's', NULL,          0,
    "Suppress idle samples to estimate CPU time."
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
    "Attach to the process with the given PID."
  },
  {
    "where",        'w', "PID",         0,
    "Dump the stacks of all the threads within the process with the given PID."
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
  {
    "pipe",         'P', NULL,          0,
    "Pipe mode. Use when piping Austin output."
  },
  {
    "gc",           'g', NULL,          0,
    "Sample the garbage collector state."
  },
  {
    "heap",         'h', "n_mb",        0,
    "Maximum heap size to allocate to increase sampling accuracy, in MB "
    "(default is 0)."
  },
  {
    "binary",       'b', NULL,          0,
    "Emit data in the MOJO binary format. "
    "See https://github.com/P403n1x87/austin/wiki/The-MOJO-file-format for more details.",
  },

  #ifdef NATIVE
  {
    "kernel",       'k', NULL,          0,
    "Sample the kernel call stack."
  },
  #endif
  #ifndef GNU_ARGP
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


#ifdef GNU_ARGP

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

  case 'b':
    pargs.binary = 1;
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

  case 'P':
    pargs.pipe = 1;
    break;

  case 'g':
    pargs.gc = 1;
    break;

  case 'h':
    if (
      fail(str_to_num(arg, (long *) &(pargs.heap))) ||
      pargs.heap > LONG_MAX
    )
      argp_error(state, "the heap size must be a positive integer");
    pargs.heap <<= 20;
    break;

  case 'w':
    if (str_to_num(arg, &l_pid) == 1 || l_pid <= 0)
      argp_error(state, "invalid PID");
    pargs.attach_pid = (pid_t) l_pid;
    pargs.where = TRUE;

    pargs.head_format = (char *) HEAD_FORMAT_WHERE;
    pargs.format = (char *) SAMPLE_FORMAT_WHERE;
    #ifdef NATIVE
    pargs.native_format = (char *) SAMPLE_FORMAT_WHERE_NATIVE;
    pargs.kernel_format = (char *) SAMPLE_FORMAT_WHERE_KERNEL;
    #endif
    break;

  #ifdef NATIVE
  case 'k':
    pargs.kernel = 1;
    break;
  #endif

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


#define argp_error(state, msg) {puts(msg);}

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
    curr_opt = _find_opt(opts, opt_str[i]);
    if (curr_opt == NULL)
      return ARG_UNRECOGNISED_OPT;

    if (curr_opt->has_arg && (equal == NULL && i < n_opts - 1))
      return ARG_MISSING_OPT_ARG;
    int cb_res = _handle_opt(curr_opt, cb, *argi, argc, argv);
    if (cb_res)
      return cb_res;
  }

  if (isvalid(curr_opt))
    *argi += curr_opt->has_arg && equal == NULL ? 2 : 1;

  return 0;
}


static const char * help_msg = \
/*[[[cog
from subprocess import check_output
for line in check_output(["src/austin", "--help"]).decode().strip().splitlines():
  print(f'"{line}\\n"')
print(";")
]]]*/
"Usage: austin [OPTION...] command [ARG...]\n"
"Austin is a frame stack sampler for CPython that is used to extract profiling\n"
"data out of a running Python process (and all its children, if required) that\n"
"requires no instrumentation and has practically no impact on the tracee.\n"
"\n"
"  -b, --binary               Emit data in the MOJO binary format. See\n"
"                             https://github.com/P403n1x87/austin/wiki/The-MOJO-file-format\n"
"                             for more details.\n"
"  -C, --children             Attach to child processes.\n"
"  -f, --full                 Produce the full set of metrics (time +mem -mem).\n"
"  -g, --gc                   Sample the garbage collector state.\n"
"  -h, --heap=n_mb            Maximum heap size to allocate to increase sampling\n"
"                             accuracy, in MB (default is 0).\n"
"  -i, --interval=n_us        Sampling interval in microseconds (default is\n"
"                             100). Accepted units: s, ms, us.\n"
"  -m, --memory               Profile memory usage.\n"
"  -o, --output=FILE          Specify an output file for the collected samples.\n"
"  -p, --pid=PID              Attach to the process with the given PID.\n"
"  -P, --pipe                 Pipe mode. Use when piping Austin output.\n"
"  -s, --sleepless            Suppress idle samples to estimate CPU time.\n"
"  -t, --timeout=n_ms         Start up wait time in milliseconds (default is\n"
"                             100). Accepted units: s, ms.\n"
"  -w, --where=PID            Dump the stacks of all the threads within the\n"
"                             process with the given PID.\n"
"  -x, --exposure=n_sec       Sample for n_sec seconds only.\n"
"  -?, --help                 Give this help list\n"
"      --usage                Give a short usage message\n"
"  -V, --version              Print program version\n"
"\n"
"Mandatory or optional arguments to long options are also mandatory or optional\n"
"for any corresponding short options.\n"
"\n"
"Report bugs to <https://github.com/P403n1x87/austin/issues>.\n"
;
/*[[[end]]]*/

static const char * usage_msg = \
/*[[[cog
from subprocess import check_output
for line in check_output(["src/austin", "--usage"]).decode().strip().splitlines():
  print(f'"{line}\\n"')
print(";")
]]]*/
"Usage: austin [-bCfgmPs?V] [-h n_mb] [-i n_us] [-o FILE] [-p PID] [-t n_ms]\n"
"            [-w PID] [-x n_sec] [--binary] [--children] [--full] [--gc]\n"
"            [--heap=n_mb] [--interval=n_us] [--memory] [--output=FILE]\n"
"            [--pid=PID] [--pipe] [--sleepless] [--timeout=n_ms] [--where=PID]\n"
"            [--exposure=n_sec] [--help] [--usage] [--version] command [ARG...]\n"
;
/*[[[end]]]*/


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
  
  case 'b':
    pargs.binary = 1;
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

  case 'w':
    if (
      str_to_num((char *) arg, (long *) &pargs.attach_pid) == 1 ||
      pargs.attach_pid <= 0
    ) {
      arg_error("invalid PID");
    }
    pargs.where = TRUE;

    pargs.head_format = (char *) HEAD_FORMAT_WHERE;
    pargs.format = (char *) SAMPLE_FORMAT_WHERE;
    break;

  case 'o':
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

  case 'P':
    pargs.pipe = 1;
    break;

  case 'g':
    pargs.gc = 1;
    break;

  case 'h':
    if (
      fail(str_to_num((char*) arg, (long *) &(pargs.heap))) ||
      pargs.heap > LONG_MAX
    )
      arg_error("the heap size must be a positive integer");
    pargs.heap <<= 20;
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


static inline void validate() {
  if (pargs.binary && pargs.where) {
    // silently ignore the binary option
    pargs.binary = 0;
  }

  if (isvalid(pargs.output_filename)) {
    pargs.output_file = fopen(pargs.output_filename, pargs.binary ? "wb" : "w");
    if (pargs.output_file == NULL) {
      puts("Unable to create the given output file");
      exit(-1);
    }
  }
  #ifdef PL_WIN
  else if (pargs.binary) {
    // Set binary mode to prevent CR/LF conversion
    setmode(fileno(pargs.output_file), O_BINARY);
  }
  #endif
}

// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
int
parse_args(int argc, char ** argv) {
  pargs.output_file = stdout;

  #ifdef GNU_ARGP
  struct argp args = {options, parse_opt, "command [ARG...]", doc};
  argp_parse(&args, argc, argv, 0, 0, 0);

  #else
  exec_arg = arg_parse(options, cb, argc, argv) - 1;
  #endif

  validate();

  return exec_arg;
}
