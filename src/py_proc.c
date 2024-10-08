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
#include <sys/mman.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "argparse.h"
#include "bin.h"
#include "events.h"
#include "py_string.h"
#include "error.h"
#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "stack.h"
#include "stats.h"
#include "timer.h"

#include "py_proc.h"
#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

#define py_proc__memcpy(self, raddr, size, dest)  copy_memory(self->proc_ref, raddr, size, dest)


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
static int
_py_proc__check_sym(py_proc_t * self, char * name, void * value) {
  if (!(isvalid(self) && isvalid(name) && isvalid(value)))
    return 0;

  for (register int i = 0; i < DYNSYM_COUNT; i++) {
    if (success(symcmp(name, i))) {
      self->symbols[i] = value;
      log_d("Symbol %s found @ %p", name, value);
      return TRUE;
    }
  }
  return FALSE;
}

// ----------------------------------------------------------------------------
static int
_get_version_from_executable(char * binary, int * major, int * minor, int * patch) {
  cu_pipe * fp;
  char      version[64];
  char      cmd[256];

  #if defined PL_WIN
  sprintf(cmd, "\"\"%s\"\" -V 2>&1", binary);
  #else
  sprintf(cmd, "%s -V 2>&1", binary);
  #endif

  fp = _popen(cmd, "r");
  if (!isvalid(fp))
    FAIL;

  while (fgets(version, sizeof(version) - 1, fp) != NULL) {
    if (sscanf(version, "Python %d.%d.%d", major, minor, patch) == 3)
      SUCCESS;
  }

  FAIL;
} /* _get_version_from_executable */

static int
_get_version_from_filename(char * filename, const char * needle, int * major, int * minor, int * patch) {
  #if defined PL_LINUX                                               /* LINUX */
  char         * base       = filename;
  char         * end        = base + strlen(base);
  size_t         needle_len = strlen(needle);

  while (base < end) {
    base = strstr(base, needle);
    if (!isvalid(base)) {
      break;
    }
    base += needle_len;
    if (sscanf(base, "%u.%u", major, minor) == 2) {
      SUCCESS;
    }
  }

  #elif defined PL_WIN                                                 /* WIN */
  // Assume the library path is of the form *.python3[0-9]+[.]dll
  int n = strlen(filename);
  if (n < 10)
    FAIL;

  char * p = filename + n - 1;
  while (*(p--) != 'n' && p > filename);
  p++;
  *major = *(p++) - '0';
  if (*major != 3)
    FAIL;

  if (sscanf(p,"%d.dll", minor) == 1)
    SUCCESS;

  #elif defined PL_MACOS                                               /* MAC */
  char * ver_needle = strstr(filename, "3.");
  if (ver_needle != NULL && sscanf(ver_needle, "%d.%d", major, minor) == 2) {
    SUCCESS;
  }  

  #endif

  FAIL;
} /* _get_version_from_filename */

#if defined PL_MACOS
static int
_find_version_in_binary(char * path, int * version) {
  size_t      binary_size = 0;
  struct stat s;

  log_d("Finding version in binary %s", path);

  cu_fd fd = open(path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary file %s", path);
    set_error(EPROC);
    FAIL;
  }

  if (fstat(fd, &s) == -1) {
    log_ie("Cannot determine size of binary file");
    set_error(EPROC);
    FAIL;
  }

  binary_size = s.st_size;

  cu_map_t * binary_map = map_new(fd, binary_size, MAP_PRIVATE);
  if (!isvalid(binary_map)) {
    log_ie("Cannot map binary file to memory");
    set_error(EPROC);
    FAIL;
  }

  char     needle[3]    = {0x00, '3', '.'};
  size_t   current_size = binary_size;
  char   * current_pos  = binary_map->addr;
  int      major, minor, patch;
  major = 0;
  while (TRUE) {
    char * p = memmem(current_pos, current_size, needle, sizeof(needle));
    if (!isvalid(p)) break;
    if (sscanf(++p, "%d.%d.%d", &major, &minor, &patch) == 3) break;
    current_size -= p - current_pos + sizeof(needle);
    current_pos = p + sizeof(needle);
  }

  if (major >= 3) {
    *version = PYVERSION(major, minor, patch);
    SUCCESS;
  }

  set_error(EPROC);
  FAIL;
} /* _find_version_in_binary */
#endif


#if defined PL_LINUX
#define LIB_NEEDLE "libpython"
#else
#define LIB_NEEDLE "python"
#endif

static int
_py_proc__infer_python_version(py_proc_t * self) {
  if (!isvalid(self)) {
    set_error(EPROC);
    FAIL;
  }

  int major = 0, minor = 0, patch = 0;

  // Starting with Python 3.13 we can use the PyRuntime structure
  if (isvalid(self->symbols[DYNSYM_RUNTIME])) {
    _Py_DebugOffsets py_d;
    if (fail(py_proc__get_type(self, self->symbols[DYNSYM_RUNTIME], py_d))) {
      log_e("Cannot copy PyRuntimeState structure from remote address");
      FAIL;
    }

    if (0 == memcmp(py_d.v3_13.cookie, "xdebugpy", sizeof(py_d.v3_13.cookie))) {
      uint64_t version = py_d.v3_13.version;
      major = (version>>24) & 0xFF;
      minor = (version>>16) & 0xFF;
      patch = (version>>8)  & 0xFF;

      log_d("Python version (from debug offsets): %d.%d.%d", major, minor, patch);

      self->py_v = get_version_descriptor(major, minor, patch);

      init_version_descriptor(self->py_v, &py_d);
      
      SUCCESS;
    }
  }

  // Starting with Python 3.11 we can rely on the Py_Version symbol
  if (isvalid(self->symbols[DYNSYM_HEX_VERSION])) {
    unsigned long py_version = 0;
    
    if (fail(py_proc__memcpy(self, self->symbols[DYNSYM_HEX_VERSION], sizeof(py_version), &py_version))) {
      log_e("Failed to dereference remote Py_Version symbol");
      FAIL;
    }
    
    major = (py_version>>24) & 0xFF;
    minor = (py_version>>16) & 0xFF;
    patch = (py_version>>8)  & 0xFF;

    log_d("Python version (from symbol): %d.%d.%d", major, minor, patch);

    self->py_v = get_version_descriptor(major, minor, patch);
    
    SUCCESS;
  }

  // Try to infer the Python version from the library file name.
  if (
    isvalid(self->lib_path)
  &&success(_get_version_from_filename(self->lib_path, LIB_NEEDLE, &major, &minor, &patch))
  ) goto from_filename;

  // On Linux, the actual executable is sometimes picked as a library. Hence we
  // try to execute the library first and see if we get a version from it. If
  // not, we fall back to the actual binary, if any.
  #if defined PL_UNIX
  if (
    isvalid(self->lib_path)
  &&(success(_get_version_from_executable(self->lib_path, &major, &minor, &patch)))
  ) goto from_exe;
  #endif

  if (
    isvalid(self->bin_path)
  &&(success(_get_version_from_executable(self->bin_path, &major, &minor, &patch)))
  ) goto from_exe;

  // Try to infer the Python version from the executable file name.
  if (
    isvalid(self->bin_path)
  &&success(_get_version_from_filename(self->bin_path, "python", &major, &minor, &patch))
  ) goto from_filename;

  #if defined PL_MACOS
  if (major == 0) {
    // We still haven't found a Python version so we look at the binary
    // content for clues
    int version;
    if (isvalid(self->bin_path) && (success(_find_version_in_binary(self->bin_path, &version)))) {
      log_d("Python version (from binary content): %d.%d.%d", major, minor, patch);
      self->py_v = get_version_descriptor(MAJOR(version), MINOR(version), PATCH(version));
      SUCCESS;
    }
  }
  #endif

  set_error(ENOVERSION);
  FAIL;

from_exe:
  log_d("Python version (from executable): %d.%d.%d", major, minor, patch);
  goto set_version;

from_filename:
  log_d("Python version (from file name): %d.%d.%d", major, minor, patch);
  goto set_version;

set_version:
  self->py_v = get_version_descriptor(major, minor, patch);
  SUCCESS;
}


// ----------------------------------------------------------------------------
static int
_py_proc__check_interp_state(py_proc_t * self, void * raddr) {
  if (!isvalid(self)) {
    set_error(EPROC);
    FAIL;
  }

  V_DESC(self->py_v);

  if (py_proc__copy_v(self, is, raddr, self->is)) {
    log_ie("Cannot get remote interpreter state");
    FAIL;
  }
  log_d("Interpreter state buffer %p", self->is);
  void * tstate_head_addr = V_FIELD_PTR(void *, self->is, py_is, o_tstate_head);

  if (fail(py_proc__copy_v(self, thread, tstate_head_addr, self->ts))) {
    log_e(
      "Cannot copy PyThreadState head at %p from PyInterpreterState instance",
      tstate_head_addr
    );
    FAIL;
  }

  log_t("PyThreadState head loaded @ %p", V_FIELD(void *, is, py_is, o_tstate_head));

  if (V_FIELD_PTR(void*, self->ts, py_thread, o_interp) != raddr) {
    log_d("PyThreadState head does not point to interpreter state");
    set_error(EPROC);
    FAIL;
  }

  log_d(
    "Found possible interpreter state @ %p (offset %p).",
    raddr, raddr - self->map.exe.base
  );

  log_t(
    "PyInterpreterState loaded @ %p. Thread State head @ %p",
    raddr, V_FIELD(void *, is, py_is, o_tstate_head)
  );

  raddr_t thread_raddr = {self->proc_ref, V_FIELD_PTR(void *, self->is, py_is, o_tstate_head)};
  py_thread_t thread;

  if (fail(py_thread__fill_from_raddr(&thread, &thread_raddr, self))) {
    log_d("Failed to fill thread structure");
    FAIL;
  }

  log_d("Stack trace constructed from possible interpreter state");

  if (V_MIN(3, 9)) {
    self->gc_state_raddr = (void *) (((char *) raddr) + py_v->py_is.o_gc);
    log_d("GC runtime state @ %p", self->gc_state_raddr);
  }

  if (V_MIN(3, 11)) {
    // In Python 3.11 we can make use of the native_thread_id field on Linux
    // to get the thread id.
    SUCCESS;
  }

  #if defined PL_LINUX
  // Try to determine the TID by reading the remote struct pthread structure.
  // We can then use this information to parse the appropriate procfs file and
  // determine the native thread's running state.
  void * initial_thread_addr = thread.raddr.addr;
  while (isvalid(thread.raddr.addr)) {
    if (success(_infer_tid_field_offset(&thread)))
      SUCCESS;
    if (is_fatal(austin_errno))
      FAIL;
    
    if (fail(py_thread__next(&thread))) {
      log_d("Failed to get next thread while inferring TID field offset");
      FAIL;
    }

    if (thread.raddr.addr == initial_thread_addr)
      break;
  }
  log_d("tid field offset not ready");
  FAIL;
  #endif /* PL_LINUX */

  SUCCESS;
}

// ----------------------------------------------------------------------------
static int
_py_proc__scan_bss(py_proc_t * self) {
  // Starting with Python 3.11, BSS scans fail because it seems that the
  // interpreter state is stored in the data section. In this case, we shift our
  // data queries into the data section. We then take steps of 64KB backwards
  // and try to find the interpreter state. This is a bit of a hack for now, but
  // it seems to work with decent performance. Note that if we fail the first
  // scan, we then look for actual interpreter states rather than pointers to
  // it. This make the search a little slower, since we now have to check every
  // value in the range. However, the step size we chose seems to get us close
  // enough in a few attempts.
  if (!isvalid(self) || !isvalid(self->map.bss.base)) {
    set_error(EPROC);
    FAIL;
  }

  cu_void * bss = malloc(self->map.bss.size);
  if (!isvalid(bss)) {
    log_e("Cannot allocate memory for BSS scan (pid: %d)", self->pid);
    set_error(EPROC);
    FAIL;
  }

  size_t step = self->map.bss.size > 0x10000 ? 0x10000 : self->map.bss.size;
  
  for (int shift = 0; shift < 1; shift++) {
    void * base = self->map.bss.base - (shift * step);
    if (fail(py_proc__memcpy(self, base, self->map.bss.size, bss))) {
      log_ie("Failed to copy BSS section");
      FAIL;
    }

    log_d("Scanning the BSS section @ %p (shift %d)", base, shift);

    void * upper_bound = bss + (shift ? step : self->map.bss.size);
    for (
      register void ** raddr = (void **) bss;
      (void *) raddr < upper_bound;
      raddr++
    ) {
      if (
        (!shift &&
        success(_py_proc__check_interp_state(self, *raddr)))
        ||(shift && success(_py_proc__check_interp_state(self, (void*) raddr - bss + base)))
      ) {
        log_d(
          "Possible interpreter state referenced by BSS @ %p (offset %x)",
          (void *) raddr - (void *) bss + (void *) base,
          (void *) raddr - (void *) bss
        );
        self->is_raddr = shift ? (void*) raddr - bss + base : *raddr;
        SUCCESS;
      }
      
      // If we don't have symbols we tolerate memory copy errors.
      if (austin_errno == EPROCNPID || (self->sym_loaded && austin_errno == EMEMCOPY))
        FAIL;
    }
    #if defined PL_WIN
    break;
    #endif
  }

  set_error(EPROC);
  FAIL;
}


// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_head(py_proc_t * self) {
  if (
    !isvalid(self)
    || !(isvalid(self->symbols[DYNSYM_RUNTIME]) || isvalid(self->map.runtime.base))
  ) {
    set_error(EPROC);
    FAIL;
  }

  V_DESC(self->py_v);
  
  void * interp_head_raddr = NULL;

  _PyRuntimeState py_runtime;
  void * runtime_addr = self->symbols[DYNSYM_RUNTIME];
  #if defined PL_LINUX
  const size_t size = getpagesize();
  #else
  const size_t size = 0;
  #endif

  void * lower = isvalid(runtime_addr) ? runtime_addr : self->map.runtime.base;
  void * upper = isvalid(runtime_addr) ? runtime_addr : lower + size;

  #ifdef DEBUG
  if (isvalid(runtime_addr)) {
    log_d("Using runtime state symbol @ %p", runtime_addr);
  }
  else {
    log_d("Using runtime state section @ %p-%p", lower, upper);
  }
  #endif

  for (void * current_addr = lower; current_addr <= upper; current_addr += sizeof(void *)) {
    if (py_proc__get_type(self, current_addr, py_runtime)) {
      log_d(
        "Cannot copy runtime state structure from remote address %p",
        current_addr
      );
      continue;
    }
    
    interp_head_raddr = V_FIELD(void *, py_runtime, py_runtime, o_interp_head);
    if (V_MAX(3, 8)) {
      self->gc_state_raddr = current_addr + py_v->py_runtime.o_gc;
      log_d("GC runtime state @ %p", self->gc_state_raddr);
    }

    if (fail(_py_proc__check_interp_state(self, interp_head_raddr))) {
      log_d("Interpreter state check failed while dereferencing runtime state");
      interp_head_raddr = NULL;
      continue;
    }
  }

  if (!isvalid(interp_head_raddr)) {
    log_d("Cannot dereference PyInterpreterState head from runtime state");
    FAIL;
  }

  self->is_raddr = interp_head_raddr;

  SUCCESS;
}


// ----------------------------------------------------------------------------
static inline void *
_py_proc__get_current_thread_state_raddr(py_proc_t * self) {
  void * p_tstate_current;

  if (self->symbols[DYNSYM_RUNTIME] != NULL) {
    if (self->tstate_current_offset == 0 || py_proc__get_type(
      self,
      self->symbols[DYNSYM_RUNTIME] + self->tstate_current_offset,
      p_tstate_current
    )) return (void *) -1;
    
    return p_tstate_current;
  }
  
  return (void *) -1;
}

// ----------------------------------------------------------------------------
static void
_py_proc__free_local_buffers(py_proc_t * self) {
  sfree(self->is);
  sfree(self->ts);
}

// ----------------------------------------------------------------------------
static int
_py_proc__init_local_buffers(py_proc_t * self) {
  if (!isvalid(self)) {
    set_error(EPROC);
    FAIL;
  }

  self->is = calloc(1, self->py_v->py_is.size);
  if (!isvalid(self->is)) {
    log_e("Cannot allocate memory for PyInterpreterState");
    goto error;
  }

  self->ts = calloc(1, self->py_v->py_thread.size);
  if (!isvalid(self->ts)) {
    log_e("Cannot allocate memory for PyThreadState");
    goto error;
  }

  log_d("Local buffers initialised");
  
  SUCCESS;

error:
  set_error(ENOMEM);

  _py_proc__free_local_buffers(self);

  FAIL;
}

// ----------------------------------------------------------------------------
static int
_py_proc__find_interpreter_state(py_proc_t * self) {
  if (!isvalid(self)) {
    set_error(EPROC);
    FAIL;
  }

  if (fail(_py_proc__init(self)))
    FAIL;

  // Determine and set version
  if (fail(_py_proc__infer_python_version(self)))
    FAIL;

  if (fail(_py_proc__init_local_buffers(self)))
    FAIL;

  if (self->sym_loaded || isvalid(self->map.runtime.base)) {
    // Try to resolve the symbols or the runtime section, if we have them

    self->is_raddr = NULL;

    if (fail(_py_proc__deref_interp_head(self))) {
      log_d("Cannot dereference PyInterpreterState head from symbols (pid: %d)", self->pid);
      FAIL;
    }
    
    log_d("✨ Interpreter head de-referenced from symbols ✨ ");
  } else {
    // Attempt a BSS scan if we don't have symbols
    if (fail(_py_proc__scan_bss(self))) {
      log_d("BSS scan failed (no symbols available)");
      FAIL;
    }
    
    log_d("Interpreter state located from BSS scan (no symbols available)");
  }

  SUCCESS;
}

// ----------------------------------------------------------------------------
static int
_py_proc__run(py_proc_t * self) {
  int try_once = self->child;
  int init = FALSE;
  int attempts = 0;

  #ifdef DEBUG
  if (!try_once)
    log_d("Start up timeout: %d ms", pargs.timeout / 1000);
  else
    log_d("Single attempt to attach to process %d", self->pid);
  #endif

  TIMER_START(pargs.timeout)
    if (try_once && ++attempts > 1) {
      log_d("Cannot attach to process %d with a single attempt.", self->pid);
      set_error(EPROC);
      FAIL;
    }

    if (!py_proc__is_running(self)) {
      log_e("Process %d is not running.", self->pid);
      set_error(EPROCNPID);
      FAIL;
    }

    sfree(self->bin_path);
    sfree(self->lib_path);
    self->sym_loaded = FALSE;

    if (success(_py_proc__find_interpreter_state(self))) {
      init = TRUE;

      log_d("Interpreter State de-referenced @ raddr: %p after %d attempts",
        self->is_raddr,
        attempts
      );

      TIMER_STOP;
    }

  TIMER_END

  log_d("_py_proc__init timer loop terminated");

  if (!init) {
    log_d("Interpreter state search timed out");
    #if defined PL_LINUX
    // This check only applies to Linux, because we don't have permission issues
    // on Windows, and if we got here on MacOS, we are already running with
    // sudo, so this is likely not a Python we can profile.
    if (austin_errno == EPROCPERM)
      // We are likely going to fail a BSS scan so we fail
      FAIL;
    #endif

    // Scan the BSS section as a last resort
    if (fail(_py_proc__scan_bss(self))) {
      log_d("BSS scan failed");
      set_error(EPROC);
      FAIL;
    }

    log_d("Interpreter state located from BSS scan");
  }

  if (!(isvalid(self->bin_path) || isvalid(self->lib_path)))
    log_w("No Python binary files detected");

  if (
    self->symbols[DYNSYM_RUNTIME] == NULL &&
    self->gc_state_raddr          == NULL
  )
    log_w("No remote symbol references have been set.");

  #ifdef DEBUG
  if (self->bin_path != NULL) log_d("Python binary:  %s", self->bin_path);
  if (self->lib_path != NULL) log_d("Python library: %s", self->lib_path);
  #endif

  self->timestamp = gettime();

  #ifdef NATIVE
  self->unwind.as = unw_create_addr_space(&_UPT_accessors, 0);
  #endif

  log_d("Python process initialization successful");

  SUCCESS;
} /* _py_proc__run */


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_proc_t *
py_proc_new(int child) {
  py_proc_t * py_proc = (py_proc_t *) calloc(1, sizeof(py_proc_t));
  if (!isvalid(py_proc))
    return NULL;

  py_proc->child = child;
  py_proc->gc_state_raddr = NULL;
  py_proc->py_v = NULL;
  
  py_proc->is = NULL;
  py_proc->ts = NULL;

  _prehash_symbols();

  py_proc->frames_heap = py_proc->frames = NULL_MEM_BLOCK;

  py_proc->frame_cache = lru_cache_new(MAX_FRAME_CACHE_SIZE, (void (*)(value_t)) frame__destroy);
  if (!isvalid(py_proc->frame_cache)) {
    log_e("Failed to allocate frame cache");
    goto error;
  }
  #ifdef DEBUG
  py_proc->frame_cache->name = "frame cache";
  #endif

  py_proc->string_cache = lru_cache_new(MAX_STRING_CACHE_SIZE, (void (*)(value_t)) free);
  if (!isvalid(py_proc->string_cache)) {
    log_e("Failed to allocate string cache");
    goto error;
  }
  #ifdef DEBUG
  py_proc->string_cache->name = "string cache";
  #endif

  py_proc->extra = (proc_extra_info *) calloc(1, sizeof(proc_extra_info));
  if (!isvalid(py_proc->extra))
    goto error;

  return py_proc;

error:
  free(py_proc);
  return NULL;
}


// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t * self, pid_t pid) {
  log_d("Attaching to process with PID %d", pid);

  #if defined PL_WIN                                                   /* WIN */
  self->proc_ref = OpenProcess(
    PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid
  );
  if (self->proc_ref == INVALID_HANDLE_VALUE) {
    set_error(EPROCATTACH);
    FAIL;
  }
  #endif                                                               /* ANY */

  self->pid = pid;

  #if defined PL_LINUX                                               /* LINUX */
  self->proc_ref = pid;
  #endif

  if (fail(_py_proc__run(self))) {
    if (austin_errno == EPROCNPID) {
      set_error(EPROCATTACH);
    }
    else {
      log_ie("Cannot attach to running process.");
    }
    FAIL;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  log_d("Starting new process using the command: %s", exec);

  #ifdef PL_WIN                                                        /* WIN */
  PROCESS_INFORMATION piProcInfo;
  STARTUPINFO         siStartInfo;
  SECURITY_ATTRIBUTES saAttr;
  HANDLE              hChildStdInRd = NULL;
  HANDLE              hChildStdInWr = NULL;
  HANDLE              hChildStdOutRd = NULL;
  HANDLE              hChildStdOutWr = NULL;

  ZeroMemory(&piProcInfo,  sizeof(PROCESS_INFORMATION));
  ZeroMemory(&siStartInfo, sizeof(STARTUPINFO));

  saAttr.nLength              = sizeof(SECURITY_ATTRIBUTES);
  saAttr.bInheritHandle       = TRUE;
  saAttr.lpSecurityDescriptor = NULL;

  CreatePipe(&hChildStdInRd, &hChildStdInWr, &saAttr, 0);
  CreatePipe(&hChildStdOutRd, &hChildStdOutWr, &saAttr, 0);

  SetHandleInformation(hChildStdInWr, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(hChildStdOutRd, HANDLE_FLAG_INHERIT, 0);

  siStartInfo.cb         = sizeof(STARTUPINFO);
  siStartInfo.hStdInput  = hChildStdInRd;
  siStartInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  siStartInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
  siStartInfo.dwFlags   |= STARTF_USESTDHANDLES;

  if (pargs.output_file == stdout) {
    log_d("Redirecting child's STDOUT to a pipe");
    siStartInfo.hStdOutput = hChildStdOutWr;

    // On Windows, Python is normally started by a launcher that duplicates the
    // standard streams, so redirecting to the NULL device causes issues. To
    // support these cases, we spawn a reader thread that reads from the pipe
    // and ensures that the buffer never gets full, stalling STDOUT operations
    // in the child process.
    DWORD dwThreadId;
    self->extra->h_reader_thread = CreateThread( 
      NULL, 0, reader_thread, hChildStdOutRd, 0, &dwThreadId
    );
    if (self->extra->h_reader_thread == NULL) {
      log_e("Failed to start STDOUT reader thread.");
      set_error(ENULLDEV);
    }
  }

  // Concatenate the command line arguments
  register int cmd_line_size = strlen(exec) + 3;  // 1 for ' ' + 2 for potential '"'s
  register int i = 1;
  while (argv[i]) cmd_line_size += strlen(argv[i++]) + 3;

  char * cmd_line = malloc(sizeof(char) * cmd_line_size);
  strcpy(cmd_line, exec);

  register int pos = strlen(exec);
  i = 1;
  while (argv[i]) {
    int has_space = isvalid(strchr(argv[i], ' '));
    cmd_line[pos++] = ' ';
    if (has_space)
      cmd_line[pos++] = '"';
    strcpy(cmd_line+pos, argv[i]);
    pos += strlen(argv[i++]);
    if (has_space)
      cmd_line[pos++] = '"';
  }
  cmd_line[pos] = '\0';

  log_t("Computed command line: %s", cmd_line);

  BOOL process_created = CreateProcess(
    NULL, cmd_line, NULL, NULL, TRUE, 0, NULL, NULL, &siStartInfo, &piProcInfo
  );

  sfree(cmd_line);

  if (!process_created) {
    log_e("CreateProcess produced error code %d", GetLastError());
    set_error(EPROCFORK);
    FAIL;
  }
  self->proc_ref = piProcInfo.hProcess;
  self->pid = (pid_t) piProcInfo.dwProcessId;

  CloseHandle(hChildStdInRd);
  CloseHandle(hChildStdOutWr);

  #else                                                               /* UNIX */
  self->pid = fork();
  if (self->pid == 0) {
    // If we are not writing to file we need to ensure the child process is
    // not writing to stdout.
    if (pargs.output_file == stdout) {
      log_d("Redirecting child's STDOUT to " NULL_DEVICE);
      if (freopen(NULL_DEVICE, "w", stdout) == NULL)
        log_e(error_get_msg(ENULLDEV));
    }

    execvpe(exec, argv, environ);

    exit(127);
  }
  #endif                                                               /* ANY */

  #if defined PL_LINUX
  self->proc_ref = self->pid;

  // On Linux we need to wait for the forked process or otherwise it will
  // become a zombie and we cannot tell with kill if it has terminated.
  pthread_create(&(self->extra->wait_thread_id), NULL, wait_thread, (void *) self);
  log_d("Wait thread created with ID %x", self->extra->wait_thread_id);
  #endif

  log_d("New process created with PID %d", self->pid);

  if (fail(_py_proc__run(self))) {
    if (austin_errno == EPROCNPID)
      set_error(EPROCFORK);
    FAIL;
  }

  #ifdef NATIVE
  self->timestamp = gettime();
  #endif

  log_d("Python process started successfully");

  SUCCESS;
}


// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t * self) {
  log_d("Waiting for process %d to terminate", self->pid);

  #if defined PL_LINUX
  if (self->extra->wait_thread_id) {
    pthread_join(self->extra->wait_thread_id, NULL);
  }
  #endif

  #ifdef PL_WIN                                                        /* WIN */
  if (isvalid(self->extra->h_reader_thread)) {
    WaitForSingleObject(self->extra->h_reader_thread, INFINITE);
    CloseHandle(self->extra->h_reader_thread);
  }
  WaitForSingleObject(self->proc_ref, INFINITE);
  CloseHandle(self->proc_ref);
  #else                                                               /* UNIX */
  #ifdef NATIVE
  wait(NULL);
  #else
  waitpid(self->pid, 0, 0);
  #endif
  #endif
}


// ----------------------------------------------------------------------------
#define PYRUNTIMESTATE_SIZE 2048  // We expect _PyRuntimeState to be < 2K.

static inline int
_py_proc__find_current_thread_offset(py_proc_t * self, void * thread_raddr) {
  if (self->symbols[DYNSYM_RUNTIME] == NULL) {
    set_error(EPROC);
    FAIL;
  }

  V_DESC(self->py_v);

  void            * interp_head_raddr;
  _PyRuntimeState   py_runtime;

  if (py_proc__get_type(self, self->symbols[DYNSYM_RUNTIME], py_runtime))
    FAIL;

  interp_head_raddr = V_FIELD(void *, py_runtime, py_runtime, o_interp_head);

  // Search offset of current thread in _PyRuntimeState structure
  PyInterpreterState is;
  py_proc__get_type(self, interp_head_raddr, is);
  void * current_thread_raddr;

  register int hit_count = 0;
  for (
    register void ** raddr = (void **) self->symbols[DYNSYM_RUNTIME];
    (void *) raddr < self->symbols[DYNSYM_RUNTIME] + PYRUNTIMESTATE_SIZE;
    raddr++
  ) {
    py_proc__get_type(self, raddr, current_thread_raddr);
    if (current_thread_raddr == thread_raddr) {
      if (++hit_count == 2) {
        self->tstate_current_offset = (void *) raddr - self->symbols[DYNSYM_RUNTIME];
        log_d(
          "Offset of _PyRuntime.gilstate.tstate_current found at %x",
          self->tstate_current_offset
        );
        SUCCESS;
      }
    }
  }

  set_error(EPROC);
  FAIL;
}


// ----------------------------------------------------------------------------
int
py_proc__is_running(py_proc_t * self) {
  #if defined PL_WIN                                                   /* WIN */
  DWORD ec = 0;
  return GetExitCodeProcess(self->proc_ref, &ec) ? ec == STILL_ACTIVE : 0;

  #elif defined PL_MACOS                                             /* MACOS */
  return success(check_pid(self->pid));

  #else                                                              /* LINUX */
  return !(kill(self->pid, 0) == -1 && errno == ESRCH);
  #endif
}


// ----------------------------------------------------------------------------
int
py_proc__is_python(py_proc_t * self) {
  return self->is_raddr != NULL;
}


// ----------------------------------------------------------------------------
static inline ssize_t
_py_proc__get_memory_delta(py_proc_t * self) {
  ssize_t current_memory = _py_proc__get_resident_memory(self);
  ssize_t delta = current_memory - self->last_resident_memory;
  self->last_resident_memory = current_memory;

  return delta;
}


// ----------------------------------------------------------------------------
int
py_proc__is_gc_collecting(py_proc_t * self) {
  if (!isvalid(self->gc_state_raddr))
    return FALSE;

  V_DESC(self->py_v);

  GCRuntimeState gc_state;
  if (fail(py_proc__get_type(self, self->gc_state_raddr, gc_state))) {
    log_d("Failed to get GC runtime state");
    return -1;
  }

  return V_FIELD(int, gc_state, py_gc, o_collecting);
}


#ifdef NATIVE
// ----------------------------------------------------------------------------
static int
_py_proc__interrupt_threads(py_proc_t * self, raddr_t * tstate_head_raddr) {
  py_thread_t py_thread;

  if (fail(py_thread__fill_from_raddr(&py_thread, tstate_head_raddr, self))) {
    FAIL;
  }

  do {
    if (pargs.kernel && fail(py_thread__save_kernel_stack(&py_thread)))
      FAIL;

    // !IMPORTANT! We need to retrieve the idle state *before* trying to
    // interrupt the thread, else it will always be idle!
    if (fail(py_thread__set_idle(&py_thread)))
      FAIL;

    if (fail(wait_ptrace(PTRACE_INTERRUPT, py_thread.tid, 0, 0))) {
      log_e("ptrace: failed to interrupt thread %d", py_thread.tid);
      set_error(EPROC);
      FAIL;
    }

    if (fail(py_thread__set_interrupted(&py_thread, TRUE))) {
      if (fail(wait_ptrace(PTRACE_CONT, py_thread.tid, 0, 0))) {
        log_d("ptrace: failed to resume interrupted thread %d (errno: %d)", py_thread.tid, errno);
      }
      FAIL;
    }
    
    log_t("ptrace: thread %d interrupted", py_thread.tid);
  } while (success(py_thread__next(&py_thread)));

  if (austin_errno != ETHREADNONEXT) {
    log_ie("Failed to iterate over threads while interrupting threads");
    FAIL;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static int
_py_proc__resume_threads(py_proc_t * self, raddr_t * tstate_head_raddr) {
  py_thread_t py_thread;

  if (fail(py_thread__fill_from_raddr(&py_thread, tstate_head_raddr, self))) {
    FAIL;
  }

  do {
    if (py_thread__is_interrupted(&py_thread)) {
      if (fail(wait_ptrace(PTRACE_CONT, py_thread.tid, 0, 0))) {
        log_d("ptrace: failed to resume thread %d (errno: %d)", py_thread.tid, errno);
        set_error(EPROC);
        FAIL;
      }
      log_t("ptrace: thread %d resumed", py_thread.tid);
      if (fail(py_thread__set_interrupted(&py_thread, FALSE))) {
        log_ie("Failed to mark thread as interrupted");
        FAIL;
      }
    }
  } while (success(py_thread__next(&py_thread)));

  if (austin_errno != ETHREADNONEXT) {
    log_ie("Failed to iterate over threads while resuming threads");
    FAIL;
  }

  SUCCESS;
}
#endif


// ----------------------------------------------------------------------------
static inline int
_py_proc__sample_interpreter(py_proc_t * self, PyInterpreterState * is, ctime_t time_delta) {
  ssize_t   mem_delta      = 0;
  void    * current_thread = NULL;

  V_DESC(self->py_v);

  void * tstate_head = V_FIELD_PTR(void *, is, py_is, o_tstate_head);
  if (!isvalid(tstate_head))
    // Maybe the interpreter state is in an invalid state. We'll try again
    // unless there is a fatal error.
    SUCCESS;
  
  raddr_t raddr = { .pref = self->proc_ref, .addr = tstate_head };
  py_thread_t py_thread;

  if (fail(py_thread__fill_from_raddr(&py_thread, &raddr, self))) {
    log_ie("Failed to fill thread from raddr while sampling");
    if (is_fatal(austin_errno)) {
      FAIL;
    }
    SUCCESS;
  }

  if (pargs.memory) {
    // Use the current thread to determine which thread is manipulating memory
    if (V_MIN(3, 12)) {
      void * gil_state_raddr = V_FIELD_PTR(void *, is, py_is, o_gil_state);
      if (!isvalid(gil_state_raddr))
        SUCCESS;
      gil_state_t gil_state;
      if (fail(copy_datatype(self->proc_ref, gil_state_raddr, gil_state))) {
        log_ie("Failed to copy GIL state");
        FAIL;
      }
      current_thread = (void *) gil_state.last_holder._value;
    }
    else
      current_thread = _py_proc__get_current_thread_state_raddr(self);
  }

  int64_t interp_id = V_FIELD_PTR(int64_t, is, py_is, o_id);
  do {
    if (pargs.memory) {
      mem_delta = 0;
      if (V_MAX(3, 11) && self->symbols[DYNSYM_RUNTIME] != NULL && current_thread == (void *) -1) {
        if (_py_proc__find_current_thread_offset(self, py_thread.raddr.addr))
          continue;
        else
          current_thread = _py_proc__get_current_thread_state_raddr(self);
      }
      if (py_thread.raddr.addr == current_thread) {
        mem_delta = _py_proc__get_memory_delta(self);
        log_t("Thread %lx holds the GIL", py_thread.tid);
      }
    }

    py_thread__emit_collapsed_stack(
      &py_thread,
      interp_id,
      time_delta,
      mem_delta
    );
  } while (success(py_thread__next(&py_thread)));

  if (austin_errno != ETHREADNONEXT) {
    log_ie("Failed to iterate over threads while sampling");
    FAIL;
  }

  SUCCESS;
} /* _py_proc__sample_interpreter */


// ----------------------------------------------------------------------------
int
py_proc__sample(py_proc_t * self) {
  ctime_t   time_delta     = gettime() - self->timestamp;  // Time delta since last sample.
  void    * current_interp = self->is_raddr;

  V_DESC(self->py_v);

  do {
    if (fail(py_proc__copy_v(self, is, current_interp, self->is))) {
      log_ie("Failed to get interpreter state while sampling");
      FAIL;
    }

    void * tstate_head = V_FIELD_PTR(void *, self->is, py_is, o_tstate_head);
    if (!isvalid(tstate_head))
      // Maybe the interpreter state is in an invalid state. We'll try again
      // unless there is a fatal error.
      SUCCESS;

    #ifdef NATIVE
    raddr_t raddr = { .pref = self->proc_ref, .addr = tstate_head };
    if (fail(_py_proc__interrupt_threads(self, &raddr))) {
      log_ie("Failed to interrupt threads");
      FAIL;
    }
    time_delta = gettime() - self->timestamp;
    #endif

    int result = _py_proc__sample_interpreter(self, self->is, time_delta);

    #ifdef NATIVE
    if (fail(_py_proc__resume_threads(self, &raddr))) {
      log_ie("Failed to resume threads");
      FAIL;
    }
    #endif
    
    if (fail(result))
      FAIL;
  } while (isvalid(current_interp = V_FIELD_PTR(void *, self->is, py_is, o_next)));
  
  #ifdef NATIVE
  self->timestamp = gettime();
  #else
  self->timestamp += time_delta;
  #endif

  SUCCESS;
} /* py_proc__sample */


// ----------------------------------------------------------------------------
void
py_proc__log_version(py_proc_t * self, int parent) {
  int major = self->py_v->major;
  int minor = self->py_v->minor;
  int patch = self->py_v->patch;

  if (pargs.pipe) {
    if (patch == 0xFF) {
      if (parent) {
        emit_metadata("python", "%d.%d.?", major, minor);
      }
      else
        log_m("# python: %d.%d.?", major, minor);
    }
    else {
      if (parent) {
        emit_metadata("python", "%d.%d.%d", major, minor, patch);
      }
      else
        log_m("# python: %d.%d.%d", major, minor, patch);
    }
  }
  else {
    log_m("");
    if (patch == 0xFF)
      log_m("🐍 \033[1mPython\033[0m version: \033[33;1m%d.%d.?\033[0m (from shared library)", major, minor);
    else
      log_m("🐍 \033[1mPython\033[0m version: \033[33;1m%d.%d.%d\033[0m", major, minor, patch);
  }
}

// ----------------------------------------------------------------------------
#if defined PL_WIN
#define SIGTERM 15
#define SIGINT  2
#endif

void
py_proc__signal(py_proc_t * self, int signal) {
  #if defined PL_WIN                                                   /* WIN */
  switch(signal) {
    case SIGINT:
      GenerateConsoleCtrlEvent(CTRL_C_EVENT, self->pid);
      break;
    case SIGTERM:
      TerminateProcess(self->proc_ref, signal);
      break;
    default:
      log_e("Cannot send signal %d to process %d", signal, self->pid);
      break;
  }
  #else                                                               /* UNIX */
  kill(self->pid, signal);
  #endif
}

// ----------------------------------------------------------------------------
void
py_proc__terminate(py_proc_t * self) {
  py_proc__signal(self, SIGTERM);
}

// ----------------------------------------------------------------------------
void
py_proc__destroy(py_proc_t * self) {
  if (!isvalid(self))
    return;

  #ifdef NATIVE
  unw_destroy_addr_space(self->unwind.as);
  vm_range_tree__destroy(self->maps_tree);
  hash_table__destroy(self->base_table);
  #endif

  _py_proc__free_local_buffers(self);

  sfree(self->bin_path);
  sfree(self->lib_path);
  sfree(self->extra);

  lru_cache__destroy(self->string_cache);
  lru_cache__destroy(self->frame_cache);

  free(self);
}
