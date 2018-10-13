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

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "austin.h"
#include "error.h"
#include "logging.h"
#include "mem.h"
#include "python.h"
#include "stats.h"

#include "py_frame.h"
#include "py_proc.h"
#include "py_thread.h"


#define DEFAULT_SAMPLING_INTERVAL    100

const char SAMPLE_FORMAT_NORMAL[]      = ";%s (%s);L%d";
const char SAMPLE_FORMAT_ALTERNATIVE[] = ";%s (%s:%d)";

static ctime_t t_sample;  // Checkmark for sampling duration calculation

// Globals for command line arguments
static ctime_t t_sampling_interval = DEFAULT_SAMPLING_INTERVAL;
static pid_t   attach_pid          = 0;
static int     exclude_empty       = 0;
static int     sleepless           = 0;
static char *  format              = (char *) SAMPLE_FORMAT_NORMAL;

static int exec_arg = 0;


// ---- HELPERS ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static void
_print_collapsed_stack(py_thread_t * thread, ctime_t delta) {
  py_frame_t * frame = py_thread__first_frame(thread);

  if (frame == NULL && exclude_empty)
    // Skip if thread has no frames and we want to exclude empty threads
    return;

  // Group entries by thread.
  printf("Thread %lx", thread->tid);

  // Append frames
  while(frame != NULL) {
    py_code_t * code = frame->code;
    if (sleepless && strstr(code->scope, "wait") != NULL) {
      delta = 0;
      printf(";<idle>");
      break;
    }
    printf(format, code->scope, code->filename, code->lineno);

    frame = frame->next;
  }

  // Finish off sample with the sampling time
  printf(" %lu\n", delta);
}


// ----------------------------------------------------------------------------
static int
_py_proc__sample(py_proc_t * py_proc) {
  ctime_t delta = gettime() - t_sample;
  error = EOK;

  PyInterpreterState is;
  if (py_proc__get_type(py_proc, py_proc->is_raddr, is) != 0)
    return 1;

  if (is.tstate_head != NULL) {
    raddr_t       raddr        = { .pid = py_proc->pid, .addr = is.tstate_head };
    py_thread_t * py_thread    = py_thread_new_from_raddr(&raddr);
    py_thread_t * first_thread = py_thread;

    while (py_thread != NULL) {
      if (py_thread->invalid) {
        printf("Bad samples %ld\n", delta);
        break;
      }

      _print_collapsed_stack(py_thread, delta);
      py_thread = py_thread__next(py_thread);
    }

    py_thread__destroy(first_thread);

    if (error != EOK)
      printf("Bad samples %ld\n", delta);
  }

  t_sample += delta;
  return 0;
}


// ----------------------------------------------------------------------------
static int
strtonum(char * str, long * num) {
  char * p_err;

  *num = strtol(str, &p_err, 10);

  return  (p_err == str || *p_err != 0) ? 1 : 0;
}


// ---- ARGUMENT PARSING ------------------------------------------------------

#if defined(__linux__)
#include <argp.h>

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


#elif defined(_WIN32) || defined(_WIN64)
#include "win/argparse.h"

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
  {0, 0, 0}
};

int cb(const char opt, const char * arg) {
  pid_t l_pid;

  switch (opt) {
  case 'i':
    if (strtonum((char *) arg, (long *) &t_sampling_interval) == 1 || t_sampling_interval < 0)
      return ARG_INVALID_VALUE;
      // arg_error(state, "the sampling interval must be a positive integer");
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
    if (strtonum((char *) arg, &l_pid) == 1 || l_pid <= 0)
      // argp_error(state, "invalid PID.");
      return ARG_INVALID_VALUE;
    attach_pid = (pid_t) l_pid;
    break;

  default:
    return ARG_STOP_PARSING;
  }

  return ARG_CONTINUE_PARSING;
}

#endif


// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
static int
parse_args(int argc, char ** argv) {
  #if defined(__linux__)
  struct argp args = {options, parse_opt, "command [ARG...]", doc};
  return argp_parse(&args, argc, argv, 0, 0, 0);

  #elif defined(_WIN32) || defined(_WIN64)
  exec_arg = arg_parse(opts, cb, argc, argv) - 1;
  return exec_arg;
  #endif
}

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int code = 0;

  parse_args(argc, argv);

  if (exec_arg == 0 && attach_pid == 0)
    return -1;

  logger_init();
  log_header();
  log_version();

  error = EOK;

  py_proc_t * py_proc = py_proc_new();
  if (py_proc == NULL)
    return EPROC;

  if (attach_pid == 0) {
    if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]) != 0)
      code = EPROCFORK;
  } else {
    if (py_proc__attach(py_proc, attach_pid) != 0)
      code = EPROCFORK;
  }

  if (code == EOK) {
    if (py_proc->is_raddr == NULL)
      code = EPROC;

    else {
      log_w("Sampling interval: %lu usec", t_sampling_interval);

      stats_reset();

      t_sample = gettime();  // Prime sample checkmark
      while(py_proc__is_running(py_proc)) {
        if (_py_proc__sample(py_proc))
          break;

        stats_check_error();

        ctime_t delta = gettime() - t_sample;  // Time spent sampling
        stats_check_duration(delta, t_sampling_interval);

        if (delta < t_sampling_interval)
          usleep(t_sampling_interval - delta);
      }

      stats_log_metrics();
    }
  }

  if (py_proc != NULL) {
    py_proc__wait(py_proc);
    py_proc__destroy(py_proc);
  }

  log_footer();
  logger_close();

  return code;
}
