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

#include <argp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "austin.h"
#include "error.h"
#include "logging.h"
#include "mem.h"
#include "stats.h"

#include "py_frame.h"
#include "py_proc.h"
#include "py_thread.h"


#define DEFAULT_SAMPLING_INTERVAL    100

static ctime_t t_sampling_interval = DEFAULT_SAMPLING_INTERVAL;
static ctime_t t_sample;  // Checkmark for sampling duration calculation

static int exec_arg = 0;


// ---- HELPERS ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static void
_print_collapsed_stack(py_thread_t * thread, ctime_t delta) {
  printf("Thread %lx", thread->tid);

  py_frame_t * frame = py_thread__first_frame(thread);
  if (frame == NULL)
    return;

  while(frame != NULL) {
    py_code_t * code = frame->code;
    printf(";%s (%s);#%d", code->scope, code->filename, code->lineno);

    frame = frame->next;
  }

  printf(" %lu\n", delta);
}


// ----------------------------------------------------------------------------
static int
_py_proc__sample(py_proc_t * py_proc) {
  PyInterpreterState is;
  if (py_proc__get_type(py_proc, py_proc->is_raddr, is) != 0)
    return 1;

  if (is.tstate_head == NULL)
    return 0;

  raddr_t       raddr        = { .pid = py_proc->pid, .addr = is.tstate_head };
  py_thread_t * py_thread    = py_thread_new_from_raddr(&raddr);
  py_thread_t * first_thread = py_thread;
  ctime_t       delta        = gettime() - t_sample;

  while (py_thread != NULL) {
    if (py_thread->invalid)
      break;
    _print_collapsed_stack(py_thread, delta);
    py_thread = py_thread__next(py_thread);
  }

  py_thread__destroy(first_thread);

  t_sample += delta;
  return 0;
}


// ---- ARGUMENT PARSING ------------------------------------------------------

const char * argp_program_version = PROGRAM_NAME " " VERSION;

const char * argp_program_bug_address = \
  "<https://github.com/P403n1x87/austin/issues>";

static const char * doc = "austin -- A frame stack sampler for Python.";

static struct argp_option options[] = {
  {"interval", 'i', "n_usec", 0, "Sampling interval (default is 500 usec)"},
  {0}
};


// ----------------------------------------------------------------------------
static int
parse_opt (int key, char *arg, struct argp_state *state)
{
  char * p_err;

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

  switch(key) {
  case 'i':
    t_sampling_interval = strtol(arg, &p_err, 10);
    if (p_err == arg || *p_err != 0)
      argp_error(state, "the sampling interval must be a positive integer");
    break;

  case ARGP_KEY_ARG:
  case ARGP_KEY_END:
    break;

  default:
    return ARGP_ERR_UNKNOWN;
  }

  return 0;
}


// ----------------------------------------------------------------------------
static int
parse_args(int argc, char ** argv) {
  struct argp args = {options, parse_opt, "command [ARG...]", doc};

  return argp_parse(&args, argc, argv, 0, 0, 0);
}


// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int code = 0;

  parse_args(argc, argv);

  if (exec_arg == 0)
    return -1;

  logger_init();
  log_header();
  log_version();

  py_proc_t * py_proc = py_proc_new();
  if (py_proc == NULL)
    return EPROC;

  if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]) != 0)
    code = EPROCFORK;

  else if (py_proc->is_raddr == NULL)
    code = EPROC;

  else {
    log_w("Sampling interval: %lu usec", t_sampling_interval);

    stats_reset();

    t_sample = gettime();  // Prime sample checkmark
    while(py_proc__is_running(py_proc)) {
      ctime_t tb = gettime();

      error = EOK;

      if (_py_proc__sample(py_proc))
        break;

      stats_check_error();

      ctime_t delta = gettime() - tb;
      stats_check_duration(delta, t_sampling_interval);

      if (delta < t_sampling_interval)
        usleep(t_sampling_interval - delta);
    }

    stats_log_metrics();
  }

  if (py_proc != NULL) {
    py_proc__wait(py_proc);
    py_proc__destroy(py_proc);
  }

  log_footer();
  logger_close();

  return code;
}
