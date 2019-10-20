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
#include "py_proc_list.h"
#include "py_thread.h"


// ---- TIMING ----------------------------------------------------------------

static ctime_t _sample_timestamp;


static void
timer_start(void) {
  _sample_timestamp = gettime();
} /* timer_start */


static void
timer_stop(void) {
  ctime_t delta = gettime() - _sample_timestamp;

  // Record stats
  stats_check_duration(delta, pargs.t_sampling_interval);

  // Pause if sampling took less than the sampling interval.
  if (delta < pargs.t_sampling_interval)
    usleep(pargs.t_sampling_interval - delta);
} /* timer_stop */


// ---- SIGNAL HANDLING -------------------------------------------------------

static int interrupt = 0;


static void
signal_callback_handler(int signum)
{
  if (signum == SIGINT)
    interrupt++;
} /* signal_callback_handler */

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
void
do_single_process(py_proc_t * py_proc) {
  while(py_proc__is_running(py_proc) && !interrupt) {
    timer_start();
    {
      py_proc__sample(py_proc);
    }
    timer_stop();
  }

  if (!interrupt)
    py_proc__wait(py_proc);

  py_proc__destroy(py_proc);
} /* do_single_process */


// ----------------------------------------------------------------------------
void
do_child_processes(py_proc_t * py_proc) {
  py_proc_list_t * list = py_proc_list_new(py_proc);
  if (list == NULL)
    return;

  // If the parent process is not running maybe it wasn't a Python
  // process. However, its children might be, so we attempt to attach
  // Austin to them.
  if (!py_proc__is_running(py_proc)) {
    log_d("Parent process is not running. Trying with its children.");

    // Since the parent process is not running we probably have waited long
    // enough so we can try to attach to child processes straight away.
    pargs.timeout = 1;

    // Store the PID before it gets deleted by the update.
    pid_t ppid = py_proc->pid;

    py_proc_list__update(list);
    py_proc_list__add_proc_children(list, ppid);
  }

  while (!py_proc_list__is_empty(list) && !interrupt) {
    timer_start();
    {
      py_proc_list__update(list);
      py_proc_list__sample(list);
    }
    timer_stop();
  }

  if (!interrupt) {
    py_proc_list__update(list);
    py_proc_list__wait(list);
  }

  py_proc_list__destroy(list);
} /* do_child_processes */


// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int retval = 0;

  int exec_arg = parse_args(argc, argv);

  if (exec_arg == 0 && pargs.attach_pid == 0) {
    retval = -1;
    goto release;
  }

  logger_init();
  log_header();
  log_version();

  if (pargs.attach_pid == 0 && argv[exec_arg] == NULL) {
    log_f("Null command and invalid PID. Austin doesn't know what to do.");
    retval = EPROC;
    goto finally;
  }

  py_proc_t * py_proc = py_proc_new();
  if (py_proc == NULL) {
    retval = EPROC;
    goto finally;
  }

  if (pargs.attach_pid == 0) {
    if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg])) {
      retval = EPROCFORK;
      goto finally;
    }
  } else {
    if (py_proc__attach(py_proc, pargs.attach_pid) && !pargs.children) {
      retval = EPROCATTACH;
      goto finally;
    }
  }

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
  {
    if (pargs.children)
      do_child_processes(py_proc);
    else
      do_single_process(py_proc);
  }
  stats_log_metrics();

finally:
  if (retval && error != EOK)
    log_i("Last error code: %d", error);
  log_footer();
  logger_close();

release:
  if (pargs.output_file != NULL && pargs.output_file != stdout) {
    fclose(pargs.output_file);
    log_d("Output file closed.");
  }

  return retval;
} /* main */
