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
#include <unistd.h>

#include "argparse.h"
#include "dict.h"
#include "error.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "stats.h"
#include "version.h"

#include "py_proc.h"
#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

// ---- Retry Timer ----
#define INIT_RETRY_SLEEP             100   /* μs */
#define INIT_RETRY_CNT                  (pargs.timeout)  /* Retry for 0.1s (default) before giving up. */

#define TIMER_RESET                     (try_cnt=gettime()+INIT_RETRY_CNT);
#define TIMER_START                     while(gettime()<=try_cnt){usleep(INIT_RETRY_SLEEP);
#define TIMER_STOP                      {try_cnt=gettime();}
#define TIMER_END                       }

static ctime_t try_cnt;


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
#define DYNSYM_COUNT                   3

#ifdef PL_MACOS
  #define SYM_PREFIX "__"
#else
  #define SYM_PREFIX "_"
#endif

static const char * _dynsym_array[DYNSYM_COUNT] = {
  SYM_PREFIX "PyThreadState_Current",
  SYM_PREFIX "PyRuntime",
  "interp_head"
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
  if (self == NULL || (self->bin_path == NULL && self->lib_path == NULL))
    return NOVERSION;

  int major = 0, minor = 0, patch = 0;


  if (self->bin_path == NULL && self->lib_path != NULL) {

    #if defined PL_LINUX                                             /* LINUX */
    if (sscanf(
        strstr(self->lib_path, "libpython"), "libpython%d.%d", &major, &minor
    ) != 2) {
      log_f("Failed to determine Python version from shared object name.");
      return NOVERSION;
    }

    #elif defined PL_WIN                                               /* WIN */
    // Assume the library path is of the form *pythonMm.dll
    int n = strlen(self->lib_path);
    major = self->lib_path[n - 6] - '0';
    minor = self->lib_path[n - 5] - '0';

    #elif defined PL_MACOS                                             /* MAC */
    char * ver_needle = strstr(self->lib_path, "/3.");
    if (ver_needle == NULL) ver_needle = strstr(self->lib_path, "/2.");
    if (ver_needle == NULL || sscanf(ver_needle, "/%d.%d", &major, &minor) != 2) {
      log_f("Failed to determine Python version from shared object path.");
      return NOVERSION;
    }
    #endif

    log_m("🐍 Python version: %d.%d.? (from shared library)", major, minor);

    return (major << 16) | (minor << 8);
  }


  FILE *fp;
  char version[64];
  char cmd[128];

  sprintf(cmd, "%s -V 2>&1", self->bin_path);

  fp = _popen(cmd, "r");
  if (fp == NULL) {
    log_f("Cannot determine the version of Python.");
    return NOVERSION;
  }

  while (fgets(version, sizeof(version) - 1, fp) != NULL) {
    if (sscanf(version, "Python %d.%d.%d", &major, &minor, &patch) == 3)
      break;
  }

  _pclose(fp);

  log_m("🐍 Python version: %d.%d.%d", major, minor, patch);

  // Scan the rodata section for something that looks like the Python version.
  // There are good chances this is at the very beginning of the section so
  // it shouldn't take too long to find a match. This is more reliable than
  // waiting until the version appears in the bss section at run-time.
  // NOTE: This method is not guaranteed to find a valid Python version.
  //       If this causes problems then another method is required.
  // char * p_ver = (char *) map + (Elf64_Addr) self->map.rodata.base;
  // for (register int i = 0; i < self->map.rodata.size; i++) {
  //   if (
  //     p_ver[i]   == '.' &&
  //     p_ver[i+1] != '.' &&
  //     p_ver[i+2] == '.' &&
  //     p_ver[i-2] == 0
  //   ) {
  //     if (
  //       sscanf(p_ver + i - 1, "%d.%d.%d", &major, &minor, &patch) == 3 &&
  //       (major == 2 || major == 3)
  //     ) {
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

  if (py_proc__get_type(self, raddr, is))
    return OUT_OF_BOUND;

  if (py_proc__get_type(self, is.tstate_head, tstate_head)) {
    log_t(
      "Cannot copy PyThreadState head at %p from PyInterpreterState instance",
      is.tstate_head
    );
    FAIL;
  }

  log_t("PyThreadState head loaded @ %p", is.tstate_head);

  if (V_FIELD(void*, tstate_head, py_thread, o_interp) != raddr)
    FAIL;

  log_d(
    "Found possible interpreter state @ %p (offset %p).",
    raddr, raddr - self->map.heap.base
  );

  log_t(
    "PyInterpreterState loaded @ %p. Thread State head @ %p",
    raddr, is.tstate_head
  );

  // As an extra sanity check, verify that the thread state is valid
  error = EOK;
  raddr_t thread_raddr = { .pid = SELF_PID, .addr = is.tstate_head };
  py_thread_t thread;
  if (!success(py_thread__fill_from_raddr(&thread, &thread_raddr))) {
    log_d("Failed to fill thread structure");
    FAIL;
  }

  if (thread.invalid) {
    log_d("... but Head Thread State is invalid!");
    FAIL;
  }

  log_d(
    "Stack trace constructed from possible interpreter state (error code: %d)",
    error
  );

  return error != EOK;
}


#ifdef CHECK_HEAP
// ----------------------------------------------------------------------------
static int
_py_proc__is_heap_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.heap.base == NULL)
    return FALSE;

  return (
    raddr >= self->map.heap.base &&
    raddr < self->map.heap.base + self->map.heap.size
  );
}


// ----------------------------------------------------------------------------
static int
_py_proc__is_raddr_within_max_range(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.heap.base == NULL)
    return FALSE;

  return (raddr >= self->min_raddr && raddr < self->max_raddr);
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
    switch (_py_proc__check_interp_state(self, raddr)) {
    case 0:
      self->is_raddr = raddr;
      SUCCESS;

    case OUT_OF_BOUND:
      return OUT_OF_BOUND;
    }
  }

  FAIL;
}
#endif


// ----------------------------------------------------------------------------
static int
_py_proc__scan_bss(py_proc_t * self) {
  if (!success(py_proc__memcpy(self, self->map.bss.base, self->map.bss.size, self->bss)))
    FAIL;

  log_d("Scanning the BSS section for PyInterpreterState");

  void * upper_bound = self->bss + self->map.bss.size;
  #ifdef CHECK_HEAP
  // When the process uses the shared library we need to search in other maps
  // other than the heap (at least on Linux). This could be optimised by
  // creating a list of all the maps and checking that a value is valid address
  // within any of these maps. However, this scan between min and max address
  // should still be relatively quick so that the extra complexity of a list is
  // not strictly required.
  int is_lib = self->lib_path != NULL;
  #endif
  for (
    register void ** raddr = (void **) self->bss;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (
      #ifdef CHECK_HEAP
        (is_lib ? _py_proc__is_raddr_within_max_range(self, *raddr)
                : _py_proc__is_heap_raddr(self, *raddr)) &&
      #endif
      _py_proc__check_interp_state(self, *raddr) == 0
    ) {
      log_d(
        "Possible interpreter state referenced by BSS @ %p (offset %x)",
        (void *) raddr - (void *) self->bss + (void *) self->map.bss.base,
        (void *) raddr - (void *) self->bss
      );
      self->is_raddr = *raddr;
      SUCCESS;
    }
  }

  FAIL;
}


#ifdef DEREF_SYM
// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_head(py_proc_t * self) {
  void * interp_head_raddr;

  if (self->py_runtime_raddr != NULL) {
    _PyRuntimeState py_runtime;
    if (py_proc__get_type(self, self->py_runtime_raddr, py_runtime)) {
      log_d(
        "Cannot copy _PyRuntimeState structure from remote address %p",
        self->py_runtime_raddr
      );
      FAIL;
    }
    interp_head_raddr = V_FIELD(void *, py_runtime, py_runtime, o_interp_head);
  }
  else if (self->interp_head_raddr != NULL) {
    if (py_proc__get_type(self, self->interp_head_raddr, interp_head_raddr)) {
      log_d(
        "Cannot copy PyInterpreterState structure from remote address %p",
        self->interp_head_raddr
      );
      FAIL;
    }
  }
  else FAIL;

  if (_py_proc__check_interp_state(self, interp_head_raddr))
    FAIL;

  self->is_raddr = interp_head_raddr;

  SUCCESS;
}


// ----------------------------------------------------------------------------
static int
_py_proc__find_interpreter_state(py_proc_t * self) {
  PyThreadState   tstate_current;
  void          * tstate_current_raddr;

  // First try to de-reference interpreter head as the most reliable method
  if (_py_proc__deref_interp_head(self)) {
    log_d("Cannot dereference PyInterpreterState head from symbols");
    // If that fails try to get the current thread state (can be NULL during idle)
    tstate_current_raddr = py_proc__get_current_thread_state_raddr(self);
    if (tstate_current_raddr == NULL || tstate_current_raddr == (void *) -1)
      // Idle or unable to dereference
      FAIL;
    else {
      if (!success(py_proc__get_type(self, tstate_current_raddr, tstate_current)))
        FAIL;

      if (!success(_py_proc__check_interp_state(
        self, V_FIELD(void*, tstate_current, py_thread, o_interp)
      ))) FAIL;

      self->is_raddr = V_FIELD(void*, tstate_current, py_thread, o_interp);
      log_d("Interpreter head de-referenced from current thread state symbol.");
    }
  } else {
    log_d("Interpreter head reference from symbol dereferenced successfully.");
  }

  SUCCESS;

  // 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
  //                 anymore, but _PyThreadState_Current.prev is.
  /* if (
      V_FIELD(void*, tstate_current, py_thread, o_thread_id) == 0 && \
      V_FIELD(void*, tstate_current, py_thread, o_prev)      != 0
    ) {
      self->tstate_curr_raddr = V_FIELD(void*, tstate_current, py_thread, o_prev);
      return 1;
    } */
}
#endif


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  #ifdef DEBUG
  register int attempts = 0;
  #endif

  TIMER_RESET
  TIMER_START
    #ifdef DEBUG
    attempts++;
    #endif

    #ifdef DEREF_SYM
    if (!success(_py_proc__find_interpreter_state(self))) {
    #endif
      if (self->bss == NULL) {
        self->bss = malloc(self->map.bss.size);
      }
      if (self->bss == NULL)
        FAIL;

      switch (_py_proc__scan_bss(self)) {
      case 0:
        log_d("Interpreter state located from BSS scan.");

      case OUT_OF_BOUND:
        TIMER_STOP
      }
    #ifdef DEREF_SYM
    } else {
      TIMER_STOP
    }
    #endif
  TIMER_END

  if (self->bss != NULL) {
    free(self->bss);
    self->bss = NULL;
  }

  // NOTE: This case should not happen anymore as the addresses have been
  //       corrected.
  //   case OUT_OF_BOUND:
  //     log_d("Symbol address not within VM maps (shared object?)");
  //     TIMER_STOP
  //     break;

  if (self->is_raddr != NULL) {
    log_d("Interpreter State de-referenced @ raddr: %p after %d attempts",
      self->is_raddr,
      attempts
    );
    SUCCESS;
  }

  #ifdef CHECK_HEAP
  log_w("BSS scan unsuccessful so we scan the heap directly ...");

  // TODO: Consider copying heap over and check for pointers
  try_cnt = 10;
  TIMER_START
    switch (_py_proc__scan_heap(self)) {
    case 0:
      SUCCESS;

    case OUT_OF_BOUND:
      TIMER_STOP
    }
  TIMER_END
  #endif

  error = EPROCISTIMEOUT;
  FAIL;
}


// ----------------------------------------------------------------------------
static int
_py_proc__run(py_proc_t * self, int try_once) {
  #ifdef DEBUG
  if (try_once == FALSE)
    log_d("Start up timeout: %d μs", pargs.timeout);
  else
    log_d("Single attempt to attach to process %d", self->pid);
  #endif

  TIMER_RESET
  TIMER_START
    if (self->bin_path != NULL) {
      free(self->bin_path);
      self->bin_path = NULL;
    }

    if (self->lib_path != NULL) {
      free(self->lib_path);
      self->lib_path = NULL;
    }

    if (_py_proc__init(self) == 0)
      break;

    if (error == EPROCPERM || error == EPROCNPID)
      FAIL;  // Fatal errors

    log_d(
      "Process not ready :: bin_path: %p, lib_path: %p, symbols: %d",
      self->bin_path, self->lib_path, self->sym_loaded
    );

    if (try_once == TRUE)
      TIMER_STOP
  TIMER_END

  if (self->bin_path == NULL && self->lib_path == NULL) {
    if (try_once == FALSE)
      log_f(
        "\n👽 No Python binaries found from process %d. Perhaps you are trying to\n"
        "start or attach to a non-Python process.", self->pid
      );
    else
      log_i("Cannot attach to process %d with just a single attempt.", self->pid);
    FAIL;
  }

  if (self->map.bss.size == 0 || self->map.bss.base == NULL)
    log_e("Unable to fully locate the BSS section.");

  if (self->min_raddr > self->max_raddr)
    log_w("Invalid remote VM maximal bounds.");

  #ifdef CHECK_HEAP
  if (self->map.heap.size == 0 || self->map.heap.base == NULL)
    log_w("Unable to fully locate the heap.");
  #endif

  #ifdef DEREF_SYM
  if (
    self->tstate_curr_raddr == NULL &&
    self->py_runtime_raddr  == NULL &&
    self->interp_head_raddr == NULL
  )
    log_w("No remote symbol references have been set.");
  #endif

  #ifdef DEBUG
  if (self->bin_path != NULL) log_d("Python binary:  %s", self->bin_path);
  if (self->lib_path != NULL) log_d("Python library: %s", self->lib_path);
  log_d("Maximal VM address space: %p-%p", self->min_raddr, self->max_raddr);
  #endif

  // Determine and set version
  if (!self->version) {
    self->version = _py_proc__get_version(self);
    if (!self->version) {
      log_f("Python version is unknown.");
      FAIL;
    }

    set_version(self->version);
  }

  if (_py_proc__wait_for_interp_state(self)) {
    log_error();
    FAIL;
  }

  self->timestamp = gettime();

  SUCCESS;
} /* _py_proc__run */


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_proc_t *
py_proc_new() {
  py_proc_t * py_proc = (py_proc_t *) calloc(1, sizeof(py_proc_t));
  if (py_proc == NULL)
    error = EPROC;

  else
    py_proc->min_raddr = (void *) -1;

  // Pre-hash symbol names
  if (_dynsym_hash_array[0] == 0) {
    for (register int i = 0; i < DYNSYM_COUNT; i++) {
      _dynsym_hash_array[i] = string_hash((char *) _dynsym_array[i]);
    }
  }

  py_proc->extra = (proc_extra_info *) calloc(1, sizeof(proc_extra_info));

  check_not_null(py_proc);
  return py_proc;
}


// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t * self, pid_t pid, int child_process) {
  log_d("Attaching to process with PID %d", pid);

  #ifdef PL_WIN                                                        /* WIN */
  self->extra->h_proc = OpenProcess(
    PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid
  );
  if (self->extra->h_proc == INVALID_HANDLE_VALUE) {
    log_e("Unable to attach to process with PID %d", pid);
    FAIL;
  }
  #endif                                                               /* ANY */

  self->pid = pid;

  return _py_proc__run(self, child_process);
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  log_d("Starting new process using the command: %s", exec);

  #ifdef PL_WIN                                                        /* WIN */
  PROCESS_INFORMATION piProcInfo;
  STARTUPINFO         siStartInfo;

  ZeroMemory(&piProcInfo,  sizeof(PROCESS_INFORMATION));
  ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));

  if (pargs.output_file == NULL) {
    HANDLE nullStdOut = CreateFile(
      TEXT(NULL_DEVICE), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL
    );

    if (nullStdOut == INVALID_HANDLE_VALUE) {
      log_e("Unable to redirect STDOUT to " NULL_DEVICE);
      return 1;
    }

    log_d("Redirecting child's STDOUT to " NULL_DEVICE);
    siStartInfo.cb         = sizeof(STARTUPINFO);
    siStartInfo.hStdOutput = nullStdOut;
    siStartInfo.dwFlags   |= STARTF_USESTDHANDLES;
  }

  // Concatenate the command line arguments
  register int cmd_line_size = strlen(exec) + 1;
  register int i = 1;
  while (argv[i]) cmd_line_size += strlen(argv[i++]) + 1;

  char * cmd_line = malloc(sizeof(char) * cmd_line_size);
  strcpy(cmd_line, exec);

  register int pos = strlen(exec);
  i = 1;
  while (argv[i]) {
    cmd_line[pos++] = ' ';
    strcpy(cmd_line+pos, argv[i]);
    pos += strlen(argv[i++]);
  }

  log_t("Computed command line: %s", cmd_line);

  BOOL process_created = CreateProcess(
    NULL, cmd_line, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo
  );

  if (cmd_line != NULL)
    free(cmd_line);

  if (!process_created) {
    log_e("Failed to create child process using the command: %s.", exec);
    FAIL;
  }
  self->extra->h_proc = piProcInfo.hProcess;
  self->pid = (pid_t) piProcInfo.dwProcessId;

  #else                                                               /* UNIX */
  self->pid = fork();
  if (self->pid == 0) {
    // If we are not writing to file we need to ensure the child process is
    // not writing to stdout.
    if (pargs.output_file == NULL) {
      log_d("Redirecting child's STDOUT to " NULL_DEVICE);
      if (freopen(NULL_DEVICE, "w", stdout) == NULL)
        log_e("Unable to redirect child's STDOUT to " NULL_DEVICE);
    }

    execvpe(exec, argv, environ);

    log_e("Failed to fork process");
    exit(127);
  }
  #endif                                                               /* ANY */

  #if defined PL_LINUX
  // On Linux we need to wait for the forked process or otherwise it will
  // become a zombie and we cannot tell with kill if it has terminated.
  pthread_create(&(self->extra->wait_thread_id), NULL, wait_thread, (void *) self);
  log_d("Wait thread created with ID %x", self->extra->wait_thread_id);
  #endif

  log_d("New process created with PID %d", self->pid);

  return _py_proc__run(self, FALSE);
}


// ----------------------------------------------------------------------------
int
py_proc__memcpy(py_proc_t * self, void * raddr, ssize_t size, void * dest) {
  return !(copy_memory(SELF_PID, raddr, size, dest) == size);
}


// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t * self) {
  log_d("Waiting for process to terminate");

  #if defined PL_LINUX
  if (self->extra->wait_thread_id) {
    pthread_join(self->extra->wait_thread_id, NULL);
  }
  #endif

  #ifdef PL_WIN                                                        /* WIN */
  WaitForSingleObject(self->extra->h_proc, INFINITE);
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
void *
py_proc__get_current_thread_state_raddr(py_proc_t * self) {
  void * p_tstate_current;

  if (self->py_runtime_raddr != NULL) {
    if (self->tstate_current_offset == 0 || py_proc__get_type(
      self,
      self->py_runtime_raddr + self->tstate_current_offset,
      p_tstate_current
    )) return (void *) -1;
  }

  else if (self->tstate_curr_raddr != NULL) {
    if (py_proc__get_type(self, self->tstate_curr_raddr, p_tstate_current))
      return (void *) -1;
  }

  else return (void *) -1;

  return p_tstate_current;
}


// ----------------------------------------------------------------------------
#define PYRUNTIMESTATE_SIZE 2048  // We expect _PyRuntimeState to be < 2K.

int
py_proc__find_current_thread_offset(py_proc_t * self, void * thread_raddr) {
  if (self->py_runtime_raddr == NULL)
    FAIL;

  void            * interp_head_raddr;
  _PyRuntimeState   py_runtime;

  if (py_proc__get_type(self, self->py_runtime_raddr, py_runtime))
    FAIL;

  interp_head_raddr = V_FIELD(void *, py_runtime, py_runtime, o_interp_head);

  // Search offset of current thread in _PyRuntimeState structure
  PyInterpreterState is;
  py_proc__get_type(self, interp_head_raddr, is);
  void * current_thread_raddr;

  register int hit_count = 0;
  for (
    register void ** raddr = (void **) self->py_runtime_raddr;
    (void *) raddr < self->py_runtime_raddr + PYRUNTIMESTATE_SIZE;
    raddr++
  ) {
    py_proc__get_type(self, raddr, current_thread_raddr);
    if (current_thread_raddr == thread_raddr) {
      if (++hit_count == 2) {
        self->tstate_current_offset = (void *) raddr - self->py_runtime_raddr;
        log_d(
          "Offset of _PyRuntime.gilstate.tstate_current found at %x",
          self->tstate_current_offset
        );
        SUCCESS;
      }
    }
  }

  FAIL;
}


// ----------------------------------------------------------------------------
int
py_proc__is_running(py_proc_t * self) {
  if (self->is_raddr == NULL)
    return FALSE;

  #ifdef PL_WIN                                                        /* WIN */
  DWORD ec = 0;
  return GetExitCodeProcess(self->extra->h_proc, &ec) ? ec == STILL_ACTIVE : 0;

  #elif defined PL_MACOS                                             /* MACOS */
  return pid_to_task(self->pid) != 0;

  #else                                                              /* LINUX */
  return !(kill(self->pid, 0) == -1 && errno == ESRCH);
  #endif
}


// ----------------------------------------------------------------------------
ssize_t
py_proc__get_memory_delta(py_proc_t * self) {
  ssize_t current_memory = _py_proc__get_resident_memory(self);
  ssize_t delta = current_memory - self->last_resident_memory;
  self->last_resident_memory = current_memory;

  return delta;
}


// ----------------------------------------------------------------------------
int
py_proc__sample(py_proc_t * self) {
  ssize_t   mem_delta = 0;
  void    * current_thread = NULL;
  ctime_t   delta = gettime() - self->timestamp;  // Time delta since last sample.

  PyInterpreterState is;
  if (!success(py_proc__get_type(self, self->is_raddr, is)))
    FAIL;

  if (is.tstate_head != NULL) {
    raddr_t raddr = { .pid = SELF_PID, .addr = is.tstate_head };
    py_thread_t py_thread;
    if (!success(py_thread__fill_from_raddr(&py_thread, &raddr)))
      FAIL;

    if (pargs.memory) {
      // Use the current thread to determine which thread is manipulating memory
      current_thread = py_proc__get_current_thread_state_raddr(self);
    }

    do {
      if (pargs.memory) {
        mem_delta = 0;
        if (self->py_runtime_raddr != NULL && current_thread == (void *) -1) {
          if (py_proc__find_current_thread_offset(self, py_thread.raddr.addr))
            continue;
          else
            current_thread = py_proc__get_current_thread_state_raddr(self);
        }
        if (py_thread.raddr.addr == current_thread) {
          mem_delta = py_proc__get_memory_delta(self);
          log_t("Thread %lx holds the GIL", py_thread.tid);
        }
      }

      py_thread__print_collapsed_stack(&py_thread, delta, mem_delta);
    } while (success(py_thread__next(&py_thread)));
  }

  self->timestamp += delta;

  SUCCESS;
} /* py_proc__sample */


// ----------------------------------------------------------------------------
void
py_proc__terminate(py_proc_t * self) {
  if (self->pid) {
    #if defined PL_UNIX
    kill(self->pid, SIGTERM);
    #else
    TerminateProcess(self->extra->h_proc, 42);
    #endif
  }
}


// ----------------------------------------------------------------------------
void
py_proc__destroy(py_proc_t * self) {
  if (self == NULL)
    return;

  if (self->bin_path != NULL)
    free(self->bin_path);

  if (self->lib_path != NULL)
    free(self->lib_path);

  if (self->bss != NULL)
    free(self->bss);

  if (self->extra != NULL)
    free(self->extra);

  free(self);
}
