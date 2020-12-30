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
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "msg.h"
#include "platform.h"
#include "python.h"
#include "stats.h"
#include "timer.h"

#include "py_proc.h"
#include "py_proc_list.h"
#include "py_thread.h"


// ---- SIGNAL HANDLING -------------------------------------------------------

static int interrupt = FALSE;


static void
signal_callback_handler(int signum)
{
  if (signum == SIGINT || signum == SIGTERM)
    interrupt = SIGTERM;
} /* signal_callback_handler */

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
void
do_single_process(py_proc_t * py_proc) {
  if (pargs.exposure == 0) {
    while(interrupt == FALSE) {
      timer_start();

      if (py_proc__sample(py_proc))
        break;

      timer_pause(timer_stop());
    }
  }
  else {
    log_m("🕑 Sampling for %d second%s", pargs.exposure, pargs.exposure != 1 ? "s" : "");
    ctime_t end_time = gettime() + pargs.exposure * 1000000;
    while(interrupt == FALSE) {
      timer_start();

      if (fail(py_proc__sample(py_proc)))
        break;

      timer_pause(timer_stop());

      if (end_time < gettime())
        interrupt++;
    }
  }

  if (interrupt == FALSE) {
    py_proc__wait(py_proc);
  }

  py_proc__destroy(py_proc);
} /* do_single_process */


// ----------------------------------------------------------------------------
void
do_child_processes(py_proc_t * py_proc) {
  py_proc_list_t * list = py_proc_list_new(py_proc);
  if (list == NULL)
    return;

  // If the parent process is not a Python process, its children might be, so we
  // attempt to attach Austin to them.
  if (!py_proc__is_python(py_proc)) {
    log_d("Parent process is not a Python process. Trying with its children.");

    // Since the parent process is not running we probably have waited long
    // enough so we can try to attach to child processes straight away.
    pargs.timeout = 1;

    // Store the PID before it gets deleted by the update.
    pid_t ppid = py_proc->pid;

    py_proc_list__update(list);
    py_proc_list__add_proc_children(list, ppid);
    if (py_proc_list__is_empty(list)) {
      set_error(pargs.attach_pid == 0 ? EPROCFORK : EPROCATTACH);
      goto release;
    }
  }

  if (pargs.exposure == 0) {
    while (!py_proc_list__is_empty(list) && interrupt == FALSE) {
      ctime_t start_time = gettime();
      py_proc_list__update(list);
      py_proc_list__sample(list);
      timer_pause(gettime() - start_time);
    }
  }
  else {
    log_m("🕑 Sampling for %d second%s", pargs.exposure, pargs.exposure != 1 ? "s" : "");
    ctime_t end_time = gettime() + pargs.exposure * 1000000;
    while (!py_proc_list__is_empty(list) && interrupt == FALSE) {
      ctime_t start_time = gettime();
      py_proc_list__update(list);
      py_proc_list__sample(list);
      timer_pause(gettime() - start_time);

      if (end_time < gettime()) interrupt++;
    }
  }

  if (interrupt == FALSE) {
    py_proc_list__update(list);
    py_proc_list__wait(list);
  }

release:
  py_proc_list__destroy(list);
} /* do_child_processes */


// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int         retval         = 0;
  py_proc_t * py_proc        = NULL;
  int         exec_arg       = parse_args(argc, argv);

  logger_init();
  log_header();
  log_version();

  if (exec_arg <= 0 && pargs.attach_pid == 0) {
    _msg(MCMDLINE);
    retval = -1;
    goto release;
  }

  if (pargs.attach_pid == 0 && argv[exec_arg] == NULL) {
    set_error(ECMDLINE);
    goto finally;
  }

  py_proc = py_proc_new();
  if (!isvalid(py_proc)) {
    log_ie("Cannot create process");
    goto finally;
  }

  if (fail(py_thread_allocate_stack())) {
    log_ie("Cannot allocate memory for thread stack");
    goto finally;
  }

  // Initialise sampling metrics.
  stats_reset();

  if (pargs.attach_pid == 0) {
    if (py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]) && !pargs.children) {
      log_ie("Cannot start the process");
      py_proc__terminate(py_proc);
      goto finally;
    }
  } else {
    if (py_proc__attach(py_proc, pargs.attach_pid, FALSE) && !pargs.children) {
      log_ie("Cannot attach the process");
      goto finally;
    }
  }

  if (error == EPROCPERM)
    goto finally;

  // Redirect output to STDOUT if not output file was given.
  if (pargs.output_file == NULL)
    pargs.output_file = stdout;
  else
    log_i("Output file: %s", pargs.output_filename);

  log_i("Sampling interval: %lu μs", pargs.t_sampling_interval);

  if (pargs.full) {
    if (pargs.memory)
      log_w("Requested full metrics. The memory switch is redundant");
    log_i("Producing full set of metrics (time +mem -mem)");
    pargs.memory = 1;
  }

  // Register signal handler for Ctrl+C and terminate signals.
  signal(SIGINT,  signal_callback_handler);
  signal(SIGTERM, signal_callback_handler);

  // Start sampling
  if (pargs.children)
    do_child_processes(py_proc);
  else
    do_single_process(py_proc);
  // The above procedures take ownership of py_proc and are responsible for
  // destroying it. Hence once they return we need to invalidate it.
  py_proc = NULL;

  if (error == EPROCNPID)
    error = EOK;

  // Log sampling metrics
  stats_log_metrics();

finally:
  py_thread_free_stack();
  sfree(py_proc);

  log_d("Last error: %d :: %s", error, get_last_error());
  if (is_fatal(error)) {
    retval = error;

    switch(error) {
    case EPROCISTIMEOUT:
      _msg(MTIMEOUT, pargs.attach_pid == 0 ? "run" : "attach to");
      break;
    #if defined PL_UNIX
    case EPROCPERM:
      _msg(MPERM);
      break;
    #endif
    case EPROCFORK:
      _msg(MFORK);
      break;
    case EPROCATTACH:
      _msg(MATTACH);
      break;
    case EPROCNPID:
      _msg(MNOPROC);
      break;
    case EPROC:
      _msg(MNOPYTHON);
      break;
    case EMEMCOPY:
      // Ignore. At this point we expect remote memory reads to fail.
      retval = EOK;
      break;
    default:
      _msg(MERROR);
    }
  }

  if (interrupt == SIGTERM)
    retval = SIGTERM;

  log_footer();
  logger_close();

release:
  if (pargs.output_file != NULL && pargs.output_file != stdout) {
    fclose(pargs.output_file);
    log_d("Output file closed.");
  }

  return retval;
} /* main */
