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
static int
_print_collapsed_stack(py_thread_t * thread, ctime_t delta, ssize_t mem_delta) {
  if (!pargs.full && pargs.memory && mem_delta <= 0)
    return 1;

  if (thread->invalid) {
    fprintf(pargs.output_file, "Thread %lx;Bad sample %ld\n", thread->tid, delta);
    return 0;
  }

  py_frame_t * frame = py_thread__first_frame(thread);

  if (frame == NULL && pargs.exclude_empty)
    // Skip if thread has no frames and we want to exclude empty threads
    return 1;

  // Group entries by thread.
  fprintf(pargs.output_file, "Thread %lx", thread->tid);

  // Append frames
  while(frame != NULL) {
    py_code_t * code = frame->code;
    if (pargs.sleepless && strstr(code->scope, "wait") != NULL) {
      delta = 0;
      fprintf(pargs.output_file, ";<idle>");
      break;
    }
    fprintf(pargs.output_file, pargs.format, code->scope, code->filename, code->lineno);

    frame = frame->next;
  }

  // Finish off sample with the metric(s)
  if (pargs.full) {
    fprintf(pargs.output_file, " %lu %ld %ld\n",
      delta, mem_delta >= 0 ? mem_delta : 0, mem_delta < 0 ? mem_delta : 0
    );
  }
  else {
    if (pargs.memory)
      fprintf(pargs.output_file, " %ld\n", mem_delta);
    else
      fprintf(pargs.output_file, " %lu\n", delta);
  }

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__sample(py_proc_t * py_proc) {
  ctime_t   delta     = gettime() - t_sample;
  ssize_t   mem_delta = 0;
  void    * current_thread;
  int       sample_printed = 0;
  error = EOK;

  PyInterpreterState is;
  if (py_proc__get_type(py_proc, py_proc->is_raddr, is) != 0)
    return 1;

  if (is.tstate_head != NULL) {
    raddr_t       raddr        = { .pid = py_proc->pid, .addr = is.tstate_head };
    py_thread_t * py_thread    = py_thread_new_from_raddr(&raddr);
    py_thread_t * first_thread = py_thread;

    if (pargs.memory) {
      // Use the current thread to determine which thread is manipulating memory
      current_thread = py_proc__get_current_thread_state_raddr(py_proc);
    }

    while (py_thread != NULL) {
      if (pargs.memory) {
        mem_delta = 0;
        if (py_proc->py_runtime_raddr != NULL && current_thread == (void *) -1) {
          if (py_proc__find_current_thread_offset(py_proc, py_thread->raddr.addr))
            continue;
          else
            current_thread = py_proc__get_current_thread_state_raddr(py_proc);
        }
        if (py_thread->raddr.addr == current_thread) {
          mem_delta = py_proc__get_memory_delta(py_proc);
          log_t("Thread %lx holds the GIL", py_thread->tid);
        }
      }
      sample_printed = !_print_collapsed_stack(py_thread, delta, mem_delta >> 10);
      py_thread = py_thread__next(py_thread);
    }

    py_thread__destroy(first_thread);

    if (error != EOK)
      fprintf(pargs.output_file, "Bad sample %ld\n", delta);
  }

  t_sample += delta;
  return !sample_printed;
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

  if (exec_arg == 0 && pargs.attach_pid == 0)
    return -1;

  logger_init();
  log_header();
  log_version();

  if (pargs.attach_pid == 0 && argv[exec_arg] == NULL) {
    log_f("Null command and invalid PID. Austin doesn't know what to do.");
    code = EPROC;
  }

  else {
    error = EOK;

    py_proc_t * py_proc = py_proc_new();
    if (py_proc == NULL)
      return EPROC;

    if (pargs.attach_pid == 0) {
      if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]) != 0)
        code = EPROCFORK;
    } else {
      if (py_proc__attach(py_proc, pargs.attach_pid) != 0)
        code = EPROCATTACH;
    }

    if (code == EOK) {
      if (py_proc->is_raddr == NULL)
        code = EPROC;

      else {
        // Register signal handler for Ctrl+C
        signal(SIGINT, signal_callback_handler);

        // Redirect output to STDOUT if not output file was given.
        if (pargs.output_file == NULL)
          pargs.output_file = stdout;
        else
          log_i("Output file: %s", pargs.output_filename);

        log_i("Sampling interval: %lu usec", pargs.t_sampling_interval);

        if (pargs.full) {
          if (pargs.memory)
            log_w("Requested full metrics. The memory switch is redundant.");
          log_i("Producing full set of metrics (time +mem -mem).");
          pargs.memory = 1;
        }

        stats_reset();

        t_sample = gettime();  // Prime sample checkmark
        while(py_proc__is_running(py_proc) && !interrupt) {
          if (_py_proc__sample(py_proc))
            continue;

          stats_check_error();

          ctime_t delta = gettime() - t_sample;  // Time spent sampling
          stats_check_duration(delta, pargs.t_sampling_interval);

          if (delta < pargs.t_sampling_interval)
            usleep(pargs.t_sampling_interval - delta);
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

  if (pargs.output_file != NULL && pargs.output_file != stdout) {
    fclose(pargs.output_file);
    log_d("Output file closed.");
  }

  log_footer();
  logger_close();

  return code;
}
