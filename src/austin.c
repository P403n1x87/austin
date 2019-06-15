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

#include "argparse.h"
#include "austin.h"
#include "error.h"
#include "logging.h"
#include "mem.h"
#include "python.h"
#include "stats.h"

#include "py_frame.h"
#include "py_proc.h"
#include "py_thread.h"


static ctime_t t_sample;  // Checkmark for sampling duration calculation


// ---- HELPERS ---------------------------------------------------------------

// ----------------------------------------------------------------------------
static void
_print_collapsed_stack(py_thread_t * thread, ctime_t delta) {
  if (thread->invalid) {
    printf("Thread %lx;Bad sample %ld\n", thread->tid, delta);
    return;
  }

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
      _print_collapsed_stack(py_thread, delta);
      py_thread = py_thread__next(py_thread);
    }

    py_thread__destroy(first_thread);

    if (error != EOK)
      printf("Bad sample %ld\n", delta);
  }

  t_sample += delta;
  return 0;
}


// ---- SIGNAL HANDLING -------------------------------------------------------

static int interrupt = 0;

static void
signal_callback_handler(int signum)
{
  if (signum == SIGINT)
    interrupt++;
}


// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int code = 0;

  int exec_arg = parse_args(argc, argv);

  if (exec_arg == 0 && attach_pid == 0)
    return -1;

  logger_init();
  log_header();
  log_version();

  if (attach_pid == 0 && argv[exec_arg] == NULL) {
    log_f("Null command and invalid PID. Austin doesn't know what to do.");
    code = EPROC;
  }

  else {
    error = EOK;

    py_proc_t * py_proc = py_proc_new();
    if (py_proc == NULL)
      return EPROC;

    if (attach_pid == 0) {
      if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]) != 0)
        code = EPROCFORK;
    } else {
      if (py_proc__attach(py_proc, attach_pid) != 0)
        code = EPROCATTACH;
    }

    if (code == EOK) {
      if (py_proc->is_raddr == NULL)
        code = EPROC;

      else {
        // Register signal handler for Ctrl+C
        signal(SIGINT, signal_callback_handler);

        log_w("Sampling interval: %lu usec", t_sampling_interval);

        stats_reset();

        t_sample = gettime();  // Prime sample checkmark
        while(py_proc__is_running(py_proc) && !interrupt) {
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
      if (!interrupt)
        py_proc__wait(py_proc);
      py_proc__destroy(py_proc);
    }
  }

  log_footer();
  logger_close();

  return code;
}
