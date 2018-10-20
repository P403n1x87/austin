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

#define PY_PROC_C

#if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
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


#define get_bounds(line, a, b) (sscanf(line, "%lx-%lx", &a, &b))

#define DYNSYM_COUNT                   2

static const char * _dynsym_array[DYNSYM_COUNT] = {
#if defined(__APPLE__) && defined(__MACH__)
  "__PyThreadState_Current",
  "__PyRuntime"
#else
  "_PyThreadState_Current",
  "_PyRuntime"
#endif
};

static long _dynsym_hash_array[DYNSYM_COUNT] = {
  0
};


// ----------------------------------------------------------------------------
#if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
#define _popen  popen
#define _pclose pclose
#endif

static int
_py_proc__get_version(py_proc_t * self) {
  if (self == NULL || self->bin_path == NULL)
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
    return -1;  // This signals that we are out of bounds.

  #ifdef DEBUG
  log_d("PyInterpreterState loaded @ %p", raddr);
  #endif

  if (py_proc__get_type(self, is.tstate_head, tstate_head) != 0)
    return 1;

  #ifdef DEBUG
  log_d("PyThreadState head loaded @ %p", is.tstate_head);
  #endif

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


// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of _py_proc__wait_for_interp_state
// ----------------------------------------------------------------------------
#if defined(__linux__)

  #include "linux/py_proc.h"

#elif defined(_WIN32) || defined(_WIN64)

  #include "win/py_proc.h"

#elif defined(__APPLE__) && defined(__MACH__)

  #include "mach/py_proc.h"

#endif
// ----------------------------------------------------------------------------


// ----------------------------------------------------------------------------
static int
_py_proc__is_bss_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.bss.base == NULL)
    return 0;

  return (raddr >= self->map.bss.base && raddr < self->map.bss.base + self->map.bss.size)
    ? 1
    : 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_state(py_proc_t * self) {
  PyThreadState   tstate_current;
  _PyRuntimeState py_runtime;

  if (self == NULL)
    return 1;

  if (self->sym_loaded == 0)
    _py_proc__analyze_bin(self);

  if (self->sym_loaded == 0)
    return 1;

  if (!self->version) {
    self->version = _py_proc__get_version(self);
    if (!self->version)
      return 1;

    set_version(self->version);
  }

  if (self->py_runtime == NULL && self->tstate_curr_raddr == NULL)
    return -2;

  // Python 3.7 exposes the _PyRuntime symbol. This can be used to find the
  // head interpreter state.
  if (self->py_runtime != NULL) {
    // NOTE: With Python 3.7, this check causes the de-reference to fail even
    //       in cases where it shouldn't.
    // if (
    //   _py_proc__is_bss_raddr(self, self->py_runtime) == 0 &&
    //   _py_proc__is_heap_raddr(self, self->py_runtime) == 0
    // ) return -1;

    if (py_proc__get_type(self, self->py_runtime, py_runtime) != 0)
      return 1;

    if (_py_proc__check_interp_state(self, py_runtime.interpreters.head))
      return 1;

    self->is_raddr = py_runtime.interpreters.head;

    return 0;
  }

  #if defined(__linux__)
  // TODO: The quality of this check on Linux is to be further assessed.
  if (
    _py_proc__is_bss_raddr(self, self->tstate_curr_raddr) == 0
    #if defined(__linux__)
      && _py_proc__is_heap_raddr(self, self->tstate_curr_raddr) == 0
    #endif
  ) return -1;
  #endif

  if (py_proc__get_type(self, self->tstate_curr_raddr, tstate_current) != 0)
    return 1;

  // 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
  //                 anymore, but _PyThreadState_Current.prev is.
  if (
    V_FIELD(void*, tstate_current, py_thread, o_thread_id) == 0 && \
    V_FIELD(void*, tstate_current, py_thread, o_prev)      != 0
  ) {
    self->tstate_curr_raddr = V_FIELD(void*, tstate_current, py_thread, o_prev);
    return 1;
  }

  if (_py_proc__check_interp_state(self, V_FIELD(void*, tstate_current, py_thread, o_interp)))
    return 1;

  self->is_raddr = V_FIELD(void*, tstate_current, py_thread, o_interp);

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_bss(py_proc_t * self) {
  if (py_proc__memcpy(self, self->map.bss.base, self->map.bss.size, self->bss))
    return 1;

  void * upper_bound = self->bss + self->map.bss.size;
  for (
    register void ** raddr = (void **) self->bss;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    #if defined(__linux__)
    if (_py_proc__is_heap_raddr(self, *raddr) && _py_proc__check_interp_state(self, *raddr) == 0) {
    #else
    if (_py_proc__check_interp_state(self, *raddr) == 0) {
    #endif
      self->is_raddr = *raddr;
      return 0;
    }
  }

  return 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  register int try_cnt = INIT_RETRY_CNT;

  while (--try_cnt) {
    usleep(INIT_RETRY_SLEEP);

    switch (_py_proc__deref_interp_state(self)) {
    case 1:
      continue;

    case 0:
      #ifdef DEBUG
      log_d("Interpreter State de-referenced @ raddr: %p after %d iterations",
        self->is_raddr,
        INIT_RETRY_CNT - try_cnt
      );
      #endif

      return 0;

    case -1:
      #ifdef DEBUG
      log_d("Symbol address not within VM maps (shared object?)");
      #endif
      try_cnt = 1;
      break;

    #if defined(__linux__)
    case -2:
      log_w("Null symbol references. This is unexpected on Linux.");
    #endif

    default:
      try_cnt = 1;
      break;
    }
  }

  if (self->version) {
    #if defined(_WIN32) || defined(_WIN64)
    log_d("Scanning the uninitialized data section ...");
    #else
    log_d("Unable to de-reference global symbols. Scanning the bss section...");
    #endif

    // Copy .bss section from remote location
    self->bss = malloc(self->map.bss.size);
    if (self->bss == NULL)
      return 1;

    try_cnt = INIT_RETRY_CNT;
    while (--try_cnt) {
      usleep(INIT_RETRY_SLEEP);

      if (_py_proc__scan_bss(self) == 0)
        return 0;
    }

    #if defined(__linux__)
    log_w("Bss scan unsuccessful. Scanning heap directly...");

    // TODO: Consider copying heap over and check for pointers
    try_cnt = INIT_RETRY_CNT;
    while (--try_cnt) {
      usleep(INIT_RETRY_SLEEP);

      if (_py_proc__scan_heap(self) == 0)
        return 0;
    }
    #endif
  } else
    log_e("Python version not set");

  error = EPROCISTIMEOUT;
  return 1;
}



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

  // Pre-hash symbol names
  if (_dynsym_hash_array[0] == 0) {
    for (register int i = 0; i < DYNSYM_COUNT; i++) {
      _dynsym_hash_array[i] = string_hash((char *) _dynsym_array[i]);
    }
  }

  check_not_null(py_proc);
  return py_proc;
}


// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t * self, pid_t pid) {
  #if defined(_WIN32) || defined(_WIN64)
  self->pid = (pid_t) OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (self->pid == (pid_t) INVALID_HANDLE_VALUE) {
    log_e("Unable to attach to process with PID %d", pid);
    return 1;
  }
  #else
  self->pid = pid;
  #endif

  if (_py_proc__wait_for_interp_state(self) == 0)
    return 0;

  log_error();
  return 1;
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  #if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
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
  if (self->pid == (pid_t) INVALID_HANDLE_VALUE) {
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
  #if defined(_WIN32) || defined(_WIN64)
  WaitForSingleObject((HANDLE) self->pid, INFINITE);
  #else
  waitpid(self->pid, 0, 0);
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
  #if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
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
