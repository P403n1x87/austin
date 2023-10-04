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

#ifndef AUSTIN_C
#define AUSTIN_C

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "argparse.h"
#include "austin.h"
#include "error.h"
#include "events.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "mojo.h"
#include "msg.h"
#include "platform.h"
#include "python/abi.h"
#include "stats.h"
#include "timing.h"
#include "version.h"

#include "py_proc.h"
#include "py_proc_list.h"
#include "py_thread.h"


// ---- SIGNAL HANDLING -------------------------------------------------------

static int interrupt = FALSE;


static void
signal_callback_handler(int signum)
{
  switch(signum) {
    case SIGINT:
    case SIGTERM:
      interrupt = -signum;
  }
} /* signal_callback_handler */

// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
void
do_single_process(py_proc_t * py_proc) {
  if (!pargs.where)
    log_meta_header();

  py_proc__log_version(py_proc, TRUE);
  if (!pargs.where) 
    NL;

  if (pargs.exposure == 0) {
    while(interrupt == FALSE) {
      stopwatch_start();

      if (fail(py_proc__sample(py_proc)))
        break;
      
      #ifdef NATIVE
      stopwatch_pause(0);
      #else
      stopwatch_pause(stopwatch_duration());
      #endif
    }
  }
  else {
    if (!pargs.where && !pargs.pipe)
      log_m("🕑 Sampling for %d second%s", pargs.exposure, pargs.exposure != 1 ? "s" : "");
    ctime_t end_time = gettime() + pargs.exposure * 1000000;
    while(interrupt == FALSE) {
      stopwatch_start();

      if (fail(py_proc__sample(py_proc)))
        break;

      #ifdef NATIVE
      stopwatch_pause(0);
      #else
      stopwatch_pause(stopwatch_duration());
      #endif

      if (end_time < gettime() || pargs.where)
        interrupt++;
    }
  }

  if (pargs.attach_pid == 0) {
    if (interrupt)
      // Propagate the signal to the parent if we spawned it.
      py_proc__signal(py_proc, interrupt < 0 ? -interrupt : SIGTERM);


    #if defined PL_UNIX
    // If we spawned the process, we need to wait for it to terminate.
    py_proc__wait(py_proc);
    #endif
  }

  py_proc__destroy(py_proc);
} /* do_single_process */


// ----------------------------------------------------------------------------
void
do_child_processes(py_proc_t * py_proc) {
  cu_py_proc_list_t * list = py_proc_list_new(py_proc);
  if (!isvalid(list))
    return;

  // If the parent process is not a Python process, its children might be, so we
  // attempt to attach Austin to them.

  if (!pargs.pipe) {
    log_m("");
    log_m("\033[1mParent process\033[0m");
  }
  if (!py_proc__is_python(py_proc)) {
    log_m("👽 Not a Python process.");

    // Since the parent process is not running we probably have waited long
    // enough so we can try to attach to child processes straight away.
    // TODO: In the future, we might want to consider adding the option to wait
    // for child processes, as they might be spawned only much later.
    pargs.timeout = 100000;  // 0.1s

    // Store the PID before it gets deleted by the update.
    pid_t ppid = py_proc->pid;

    py_proc_list__update(list);
    py_proc_list__add_proc_children(list, ppid);

    if (py_proc_list__size(list) == 1) {
      set_error(EPROCNOCHILDREN);
      if (pargs.attach_pid == 0)
        py_proc__terminate(py_proc);
      return;
    }
  }
  else {
    py_proc__log_version(py_proc, TRUE);
  }

  if (!py_proc_list__is_empty(list) && interrupt == FALSE) {
    if (!pargs.pipe) {
      log_m("");
      log_m("\033[1mChild processes\033[0m");
    }
  }

  if (!pargs.where) {
    log_meta_header();
    NL;
  }

  if (pargs.exposure == 0) {
    while (!py_proc_list__is_empty(list) && interrupt == FALSE) {
      #ifndef NATIVE
      ctime_t start_time = gettime();
      #endif
      py_proc_list__update(list);
      py_proc_list__sample(list);
      #ifdef NATIVE
      stopwatch_pause(0);
      #else
      stopwatch_pause(gettime() - start_time);
      #endif
    }
  }
  else {
    if (!pargs.pipe && !pargs.where)
      log_m("🕑 Sampling for %d second%s", pargs.exposure, pargs.exposure != 1 ? "s" : "");
    ctime_t end_time = gettime() + pargs.exposure * 1000000;
    while (!py_proc_list__is_empty(list) && interrupt == FALSE) {
      #ifndef NATIVE
      ctime_t start_time = gettime();
      #endif
      py_proc_list__update(list);
      py_proc_list__sample(list);
      #ifdef NATIVE
      stopwatch_pause(0);
      #else
      stopwatch_pause(gettime() - start_time);
      #endif

      if (end_time < gettime() || pargs.where)
        interrupt++;
    }
  }

  if (pargs.attach_pid == 0) {
    if (interrupt)
      // Propagate the signal to the child processes (via the parent) if we
      // spawned them.
      py_proc__signal(py_proc, interrupt < 0 ? -interrupt : SIGTERM);

    // If we spawned the child processes, we need to wait for them to terminate.
    py_proc_list__update(list);
    #if defined PL_UNIX
    py_proc_list__wait(list);
    #endif
  }
} /* do_child_processes */


// ----------------------------------------------------------------------------
static inline int
handle_error() {  
  log_d("Last error: %d :: %s", austin_errno, get_last_error());
  
  int retval = austin_errno;

  switch(retval) {
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
  case EPROCNOCHILDREN:
    _msg(MNOCHILDREN);
    break;
  case ENOVERSION:
    _msg(MNOVERSION);
    break;
  case EMEMCOPY:
    // Ignore. At this point we expect remote memory reads to fail.
    retval = EOK;
    break;
  default:
    _msg(MERROR);
  }

  return retval;
} /* handle_error */

// ---- MAIN ------------------------------------------------------------------

// ----------------------------------------------------------------------------
int main(int argc, char ** argv) {
  int         retval         = 0;
  py_proc_t * py_proc        = NULL;
  int         exec_arg       = parse_args(argc, argv);

#if defined PL_MACOS
  // On MacOS, we need to be root to use Austin.
  if (geteuid() != 0) {
    _msg(MPERM);
    return EPROCPERM;
  }
#endif

  logger_init();
  if (!pargs.pipe)
    log_header();

  if (exec_arg <= 0 && pargs.attach_pid == 0) {
    _msg(MCMDLINE);
    retval = -1;
    goto release;
  }

  if (pargs.attach_pid == 0 && argv[exec_arg] == NULL) {
    set_error(ECMDLINE);
    goto finally;
  }

  py_proc = py_proc_new(FALSE);
  if (!isvalid(py_proc)) {
    log_ie("Cannot create process");
    goto finally;
  }

  if (fail(py_thread_allocate())) {
    log_ie("Cannot allocate memory for thread stack");
    goto finally;
  }

  // Initialise sampling metrics.
  stats_reset();

  if (pargs.binary) {
    mojo_header();
  }

  if (pargs.attach_pid == 0) {
    if (
      (fail(py_proc__start(py_proc, argv[exec_arg], (char **) &argv[exec_arg]))
      && !pargs.children)
      || py_proc->pid == 0
    ) {
      log_ie("Cannot start the process");
      py_proc__terminate(py_proc);

      retval = handle_error();
      goto finally;
    }
  } else {
    if (
      fail(py_proc__attach(py_proc, pargs.attach_pid))
      && !pargs.children
    ) {
      log_ie("Cannot attach the process");

      retval = handle_error();
      goto finally;
    }
  }

  // Redirect output to STDOUT if not output file was given.
  if (pargs.output_file != stdout)
    log_i("Output file: %s", pargs.output_filename);

  if (pargs.where) {
    log_i("Where mode on process %d", pargs.attach_pid);
    pargs.t_sampling_interval = 1;
    // We use the exposure branch to emulate sampling once
    pargs.exposure = 1;
  }
  else
    log_i("Sampling interval: %lu μs", pargs.t_sampling_interval);

  if (pargs.heap)
    log_i("Maximum frame heap size: %d MB", pargs.heap >> 20);

  if (pargs.full) {
    if (pargs.memory)
      log_w("The memory switch is redundant in full mode");
    if (pargs.sleepless)
      log_w("The sleepless switch is redundant in full mode");
    log_i("Producing full set of metrics (time +mem -mem)");
    pargs.memory = TRUE;
  }
  else if (pargs.memory) {
    if (pargs.sleepless)
      log_w("The sleepless switch is incompatible with memory mode.");
    pargs.sleepless = FALSE;
  }

  // Register signal handler for Ctrl+C and terminate signals.
  signal(SIGINT,  signal_callback_handler);
  signal(SIGTERM, signal_callback_handler);

  stats_start();


  // Start sampling
  if (pargs.children) {
    do_child_processes(py_proc);
  }
  else {
    do_single_process(py_proc);
  }
  // The above procedures take ownership of py_proc and are responsible for
  // destroying it. Hence once they return we need to invalidate it.
  py_proc = NULL;

  if (austin_errno == EPROCNOCHILDREN) {
    retval = handle_error();
    goto finally;
  }

  if (pargs.where)
    goto finally;

  // Log sampling metrics
  NL;
  
  emit_metadata("duration", "%lu", stats_duration());
  if (pargs.gc) {
    emit_metadata("gc", "%lu", _gc_time);
  }

  stats_log_metrics();NL;

finally:
  py_thread_free();
  py_proc__destroy(py_proc);

  log_footer();

release:
  if (pargs.output_file != NULL && pargs.output_file != stdout) {
    fclose(pargs.output_file);
    log_d("Output file closed.");
  }

  logger_close();

  if (interrupt < 0)
    // Interrupted  by signal
    retval = interrupt;

  log_d("Exiting with code %d", retval);

  return retval;
} /* main */

#endif
