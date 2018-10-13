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

#if defined(__linux__)
#include <sys/wait.h>
#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dict.h"
#include "error.h"
#include "logging.h"
#include "mem.h"
#include "version.h"

#include "py_proc.h"
#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

#define INIT_RETRY_SLEEP             100  /* usecs */
#define INIT_RETRY_CNT              1000  /* Retry for 0.1s before giving up. */


// Get the offset of the ith section header
#define get_bounds(line, a, b) (sscanf(line, "%lx-%lx", &a, &b))


// ----------------------------------------------------------------------------
#if defined(__linux__)
#define _popen  popen
#define _pclose pclose
#endif

static int
_py_proc__get_version(py_proc_t * self) {
  if (self == NULL)
    return 0;

  int major = 0, minor = 0, patch = 0;

  FILE *fp;
  char version[64];
  char cmd[128];

  sprintf(cmd, "%s -V 2>&1", self->bin_path);

  fp = _popen(cmd, "r");
  if (fp == NULL) {
    log_e("Failed to start Python to determine its version.");
    return 0;
  }

  while (fgets(version, sizeof(version) - 1, fp) != NULL) {
    if (sscanf(version, "Python %d.%d.%d", &major, &minor, &patch) == 3)
      break;
  }

  /* close */
  _pclose(fp);

  log_i("Python version: %d.%d.%d", major, minor, patch);

  // Scan the rodata section for something that looks like the Python version.
  // There are good chances this is at the very beginning of the section so
  // it shouldn't take too long to find a match. This is more reliable than
  // waiting until the version appears in the bss section at run-time.
  // NOTE: This method is not guaranteed to find a valid Python version.
  //       If this causes problems then another method is required.
  // char * p_ver = (char *) map + (Elf64_Addr) self->map.rodata.base;
  // for (register int i = 0; i < self->map.rodata.size; i++) {
  //   if (p_ver[i] == '.' && p_ver[i+1] != '.' && p_ver[i+2] == '.' && p_ver[i-2] == 0) {
  //     if (sscanf(p_ver + i - 1, "%d.%d.%d", &major, &minor, &patch) == 3 && (major == 2 || major == 3)) {
  //       log_i("Python version: %s", p_ver + i - 1, p_ver);
  //       // break;
  //     }
  //   }
  // }

  return (major << 16) | (minor << 8) | patch;
}


// ----------------------------------------------------------------------------
static int
_py_proc__check_interp_state(py_proc_t * self, void * raddr) {
  PyInterpreterState is;
  PyThreadState      tstate_head;

  if (py_proc__get_type(self, raddr, is) != 0)
    return -1; // This signals that we are out of bounds.

  #ifdef DEBUG
  // log_d("PyInterpreterState loaded @ %p", raddr);
  #endif

  if (py_proc__get_type(self, is.tstate_head, tstate_head) != 0)
    return 1;

  #ifdef DEBUG
  // log_d("PyThreadState head loaded @ %p", is.tstate_head);
  #endif

  if (V_FIELD(void*, tstate_head, py_thread, o_interp) != NULL)
    log_d(
      "ThreadState Head: interp: %p, frame: %p",
      V_FIELD(void*, tstate_head, py_thread, o_interp),
      V_FIELD(void*, tstate_head, py_thread, o_frame)
    );

  if (
    (V_FIELD(void*, tstate_head, py_thread, o_interp)) != raddr ||
    (V_FIELD(void*, tstate_head, py_thread, o_frame))  == 0
  )
    return 1;

  #ifdef DEBUG
  log_d("Found possible interpreter state @ %p (offset %p).", raddr, raddr - self->map.heap.base);
  #endif

  error = EOK;
  raddr_t thread_raddr = { .pid = self->pid, .addr = is.tstate_head };
  py_thread_t * thread = py_thread_new_from_raddr(&thread_raddr);
  if (thread == NULL)
    return 1;
  py_thread__destroy(thread);

  #ifdef DEBUG
  log_d("Stack trace constructed from possible interpreter state (error %d)", error);
  #endif

  return error == EOK ? 0 : 1;
}


// Platform-dependent implementations of _py_proc__wait_for_interp_state
#if defined(__linux__)
#include "linux/py_proc.c"
#elif defined(_WIN32) || defined(_WIN64)
#include "win/py_proc.c"
#endif


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_proc_t *
py_proc_new() {
  py_proc_t * py_proc = (py_proc_t *) malloc(sizeof(py_proc_t));
  if (py_proc == NULL)
    error = EPROC;

  else {
    py_proc->pid      = 0;
    py_proc->bin_path = NULL;
    py_proc->is_raddr = NULL;

    py_proc->bss = NULL;

    py_proc->maps_loaded       = 0;
    py_proc->sym_loaded        = 0;
    py_proc->tstate_curr_raddr = NULL;
    py_proc->py_runtime        = NULL;

    py_proc->version = 0;
  }

  check_not_null(py_proc);
  return py_proc;
}


// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t * self, pid_t pid) {
  self->pid = pid;

  if (_py_proc__wait_for_interp_state(self) == 0)
    return 0;

  log_error();
  return 1;
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  #if defined(__linux__)
  self->pid = fork();

  if (self->pid == 0) {
    execvp(exec, argv);
    log_e("Failed to fork process");
    exit(127);
  }
  else {
    if (_py_proc__wait_for_interp_state(self) == 0)
      return 0;
  }

  #elif defined(_WIN32) || defined(_WIN64)
  self->pid = _spawnvp(_P_NOWAIT, exec, (const char * const*) argv);
  if (self->pid == INVALID_HANDLE_VALUE) {
    log_e("Failed to spawn command: %s", exec);
    return 1;
  }

  if (_py_proc__wait_for_interp_state(self) == 0)
    return 0;
  #endif

  log_error();
  return 1;

}


// ----------------------------------------------------------------------------
int
py_proc__memcpy(py_proc_t * self, void * raddr, ssize_t size, void * dest) {
  return copy_memory(self->pid, raddr, size, dest) == size ? 0 : 1;
}


// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t * self) {
  #ifdef DEBUG
  log_d("Waiting for process to terminate");
  #endif
  #if defined(__linux__)
  waitpid(self->pid, 0, 0);
  #elif defined(_WIN32) || defined(_WIN64)
  WaitForSingleObject((HANDLE) self->pid, INFINITE);
  #endif
}


// ----------------------------------------------------------------------------
void *
py_proc__get_istate_raddr(py_proc_t * self) {
  return self->is_raddr;
}


// ----------------------------------------------------------------------------
int
py_proc__is_running(py_proc_t * self) {
  #if defined(__linux__)
  kill(self->pid, 0);
  return errno == ESRCH ? 0 : 1;

  #elif defined(_WIN32) || defined(_WIN64)
  DWORD ec;
  return GetExitCodeProcess((HANDLE) self->pid, &ec) ? ec : -1;
  #endif
}


// ----------------------------------------------------------------------------
void
py_proc__destroy(py_proc_t * self) {
  if (self == NULL)
    return;

  if (self->bin_path != NULL)
    free(self->bin_path);

  if (self->bss != NULL)
    free(self->bss);

  free(self);
}
