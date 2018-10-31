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

#include "platform.h"

#ifdef PL_WIN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
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

// ---- Retry Timer ----
#define INIT_RETRY_SLEEP             100  /* usecs */
#define INIT_RETRY_CNT              1000  /* Retry for 0.1s before giving up. */

#define TIMER_RESET (try_cnt = INIT_RETRY_CNT);
#define TIMER_START while (--try_cnt) { usleep(INIT_RETRY_SLEEP);
#define TIMER_STOP  (try_cnt = 1);
#define TIMER_END   }

static int try_cnt = INIT_RETRY_CNT;


// ----------------------------------------------------------------------------
// -- Platform-dependent implementations of _py_proc__init
// ----------------------------------------------------------------------------

// Forward declaration
static int _py_proc__check_sym(py_proc_t *, char *, void *);

#if defined(PL_LINUX)

  #include "linux/py_proc.h"

#elif defined(PL_WIN)

  #include "win/py_proc.h"

#elif defined(PL_MACOS)

  #include "mac/py_proc.h"

#endif
// ----------------------------------------------------------------------------


// ---- Exported symbols ----
#define DYNSYM_COUNT                   2

#ifdef PL_MACOS
  #define SYM_PREFIX "__"
#else
  #define SYM_PREFIX "_"
#endif

static const char * _dynsym_array[DYNSYM_COUNT] = {
  SYM_PREFIX "PyThreadState_Current",
  SYM_PREFIX "PyRuntime"
};

static long _dynsym_hash_array[DYNSYM_COUNT] = {
  0
};


#ifdef DEREF_SYM
static int
_py_proc__check_sym(py_proc_t * self, char * name, void * value) {
  for (register int i = 0; i < DYNSYM_COUNT; i++) {
    if (
      string_hash(name) == _dynsym_hash_array[i] &&
      strcmp(name, _dynsym_array[i]) == 0
    ) {
      *(&(self->tstate_curr_raddr) + i) = value;
      log_d("Symbol %s found @ %p", name, value);
      return 1;
    }
  }
  return 0;
}
#endif

// ----------------------------------------------------------------------------
#ifdef PL_UNIX
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

  log_t("PyInterpreterState loaded @ %p", raddr);

  if (py_proc__get_type(self, is.tstate_head, tstate_head) != 0)
    return 1;

  log_t("PyThreadState head loaded @ %p", is.tstate_head);

  if (V_FIELD(void*, tstate_head, py_thread, o_interp) != raddr)
    return 1;

  log_d("Found possible interpreter state @ %p (offset %p).", raddr, raddr - self->map.heap.base);

  error = EOK;
  raddr_t thread_raddr = { .pid = self->pid, .addr = is.tstate_head };
  py_thread_t * thread = py_thread_new_from_raddr(&thread_raddr);
  if (thread == NULL)
    return 1;
  py_thread__destroy(thread);

  log_d("Stack trace constructed from possible interpreter state (error %d)", error);

  return error != EOK;
}


#ifdef CHECK_HEAP
// ----------------------------------------------------------------------------
static int
_py_proc__is_heap_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.heap.base == NULL)
    return 0;

  return (raddr >= self->map.heap.base && raddr < self->map.heap.base + self->map.heap.size);
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_heap(py_proc_t * self) {
  // NOTE: This seems to be required by Python 2.7 on i386 Linux.
  void * upper_bound = self->map.heap.base + self->map.heap.size;
  for (
    register void ** raddr = (void **) self->map.heap.base;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (_py_proc__check_interp_state(self, raddr) == 0) {
      self->is_raddr = raddr;
      return 0;
    }
  }

  return 1;
}
#endif


#ifdef PL_LINUX
// ----------------------------------------------------------------------------
static int
_py_proc__is_bss_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.bss.base == NULL)
    return 0;

  return (raddr >= self->map.bss.base && raddr < self->map.bss.base + self->map.bss.size);
}
#endif


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
    #ifdef CHECK_HEAP
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


#ifdef DEREF_SYM
// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_state(py_proc_t * self) {
  PyThreadState   tstate_current;
  _PyRuntimeState py_runtime;

  if (self == NULL)
    return 1;

  if (self->py_runtime_raddr == NULL && self->tstate_curr_raddr == NULL)
    return -2;

  // Python 3.7 exposes the _PyRuntime symbol. This can be used to find the
  // head interpreter state.
  if (self->py_runtime_raddr != NULL) {
    // NOTE: With Python 3.7, this check causes the de-reference to fail even
    //       in cases where it shouldn't.
    // if (
    //   _py_proc__is_bss_raddr(self, self->py_runtime_raddr) == 0 &&
    //   _py_proc__is_heap_raddr(self, self->py_runtime_raddr) == 0
    // ) return -1;

    if (py_proc__get_type(self, self->py_runtime_raddr, py_runtime) != 0)
      return 1;

    if (_py_proc__check_interp_state(self, py_runtime.interpreters.head))
      return 1;

    self->is_raddr = py_runtime.interpreters.head;

    return 0;
  }

  // TODO: The quality of this check on Linux is to be further assessed.
  #ifdef PL_LINUX
  if (
    _py_proc__is_bss_raddr(self, self->tstate_curr_raddr) == 0 &&
    _py_proc__is_heap_raddr(self, self->tstate_curr_raddr) == 0
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
#endif


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  #ifdef DEREF_SYM
  TIMER_RESET
  TIMER_START
    switch (_py_proc__deref_interp_state(self)) {
    case 1:
      continue;

    case 0:
      log_d("Interpreter State de-referenced @ raddr: %p after %d iterations",
        self->is_raddr,
        INIT_RETRY_CNT - try_cnt
      );
      return 0;

    case -1:
      log_d("Symbol address not within VM maps (shared object?)");
      TIMER_STOP
      break;

    #ifdef DEREF_SYM
    case -2:
      log_w("Null symbol references. This is unexpected.");
    #endif

    default:
      TIMER_STOP
      break;
    }
  TIMER_END
  #endif

  if (self->map.bss.size == 0) {
    log_f("Process not properly initialised. Invalid BSS reference found.");
    error = EPROCVM;
    return 1;
  }

  #ifdef DEREF_SYM
  log_d("Unable to de-reference global symbols. Scanning the bss section...");
  #else
  log_d("Scanning the uninitialized data section ...");
  #endif

  // Copy .bss section from remote location
  self->bss = malloc(self->map.bss.size);
  if (self->bss == NULL)
    return 1;

  TIMER_RESET
  TIMER_START
    if (_py_proc__scan_bss(self) == 0)
      return 0;
  TIMER_END

  #ifdef CHECK_HEAP
  log_w("Bss scan unsuccessful. Scanning heap directly...");

  // TODO: Consider copying heap over and check for pointers
  TIMER_RESET
  TIMER_START
    if (_py_proc__scan_heap(self) == 0)
      return 0;
  TIMER_END
  #endif

  error = EPROCISTIMEOUT;
  return 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__run(py_proc_t * self) {
  TIMER_START
    if (_py_proc__init(self) == 0)
      break;
  TIMER_END

  if (self->bin_path == NULL) {
    log_f("Python binary not found. Not Python?");
    return 1;
  }

  if (self->map.bss.size == 0)
    log_e("Invalid BSS structure.");

  #ifdef CHECK_HEAP
  if (self->map.heap.size == 0)
    log_e("Invalid HEAP structure.");
  #endif

  #ifdef DEREF_SYM
  if (self->tstate_curr_raddr == NULL && self->py_runtime_raddr == NULL)
    log_e("Expecting valid symbol references. Found none.");
  #endif

  log_d("Python binary: %s", self->bin_path);

  // Determine and set version
  if (!self->version) {
    self->version = _py_proc__get_version(self);
    if (!self->version) {
      log_f("Python version is unknown.");
      return 1;
    }

    set_version(self->version);
  }

  if (_py_proc__wait_for_interp_state(self)) {
    log_error();
    return 1;
  }

  return 0;
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

    py_proc->map.bss.base = NULL;
    py_proc->map.bss.size = 0;

    py_proc->bss = NULL;

    py_proc->maps_loaded = 0;
    py_proc->sym_loaded  = 0;

    py_proc->tstate_curr_raddr = NULL;
    py_proc->py_runtime_raddr  = NULL;

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
  log_d("Attaching to process with PID %d", pid);
  #ifdef PL_WIN
  self->pid = (pid_t) OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (self->pid == (pid_t) INVALID_HANDLE_VALUE) {
    log_e("Unable to attach to process with PID %d", pid);
    return 1;
  }
  #else
  self->pid = pid;
  #endif

  return _py_proc__run(self);
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  log_d("Starting new process with command: %s", exec);

  #ifdef PL_WIN                                                        /* WIN */
  self->pid = _spawnvp(_P_NOWAIT, exec, (const char * const*) argv);
  if (self->pid == (pid_t) INVALID_HANDLE_VALUE) {
    log_e("Failed to spawn command: %s", exec);
    return 1;
  }
  #else                                                               /* UNIX */
  self->pid = fork();
  if (self->pid == 0) {
    execvp(exec, argv);
    log_e("Failed to fork process");
    exit(127);
  }
  #endif                                                               /* ANY */

  return _py_proc__run(self);
}


// ----------------------------------------------------------------------------
int
py_proc__memcpy(py_proc_t * self, void * raddr, ssize_t size, void * dest) {
  return copy_memory(self->pid, raddr, size, dest) == size ? 0 : 1;
}


// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t * self) {
  log_d("Waiting for process to terminate");

  #ifdef PL_WIN                                                        /* WIN */
  WaitForSingleObject((HANDLE) self->pid, INFINITE);
  #else                                                               /* UNIX */
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
  #ifdef PL_WIN                                                        /* WIN */
  DWORD ec;
  return GetExitCodeProcess((HANDLE) self->pid, &ec) ? ec : -1;

  #else                                                               /* UNIX */
  kill(self->pid, 0);
  return errno == ESRCH ? 0 : 1;
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
