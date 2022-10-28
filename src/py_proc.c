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

#include "py_proc.h"
#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

// ---- Retry Timer ----
#define INIT_RETRY_SLEEP             100   /* μs */
#define INIT_TIMER_INTERVAL             (pargs.timeout)  /* Retry for 0.1s (default) before giving up. */

#define TIMER_SET(x)                    {_end_time=gettime()+(x);}
#define TIMER_RESET                     {_end_time=gettime()+INIT_TIMER_INTERVAL;}
#define TIMER_START                     while(gettime()<=_end_time){usleep(INIT_RETRY_SLEEP);
#define TIMER_STOP                      {_end_time=0;}
#define TIMER_END                       }

static ctime_t _end_time;


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


#ifdef DEREF_SYM
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
#endif


// ----------------------------------------------------------------------------
#ifdef PL_UNIX
#define _popen  popen
#define _pclose pclose
#endif

static int
_get_version_from_executable(char * binary, int * major, int * minor, int * patch) {
  FILE * fp;
  char   version[64];
  char   cmd[256];

  #if defined PL_WIN
  sprintf(cmd, "\"\"%s\"\" -V 2>&1", binary);
  #else
  sprintf(cmd, "%s -V 2>&1", binary);
  #endif

  fp = _popen(cmd, "r");
  if (!isvalid(fp)) {
    FAIL;
  }

  with_resources;

  while (fgets(version, sizeof(version) - 1, fp) != NULL) {
    if (sscanf(version, "Python %d.%d.%d", major, minor, patch) == 3)
      OK;
  }

  NOK;

release:
  _pclose(fp);

  released;
} /* _get_version_from_executable */

static int
_get_version_from_filename(char * filename, int * major, int * minor, int * patch) {
  #if defined PL_LINUX                                               /* LINUX */
  char         * base       = filename;
  char         * end        = base + strlen(base);
  const char   * needle     = "python";
  const size_t   needle_len = strlen(needle);

  while (base < end) {
    base = strstr(base, needle);
    if (!isvalid(base)) {
      break;
    }
    base += needle_len;
    if (sscanf(base,"%d.%d", major, minor) == 2) {
      SUCCESS;
    }
  }

  #elif defined PL_WIN                                                 /* WIN */
  // Assume the library path is of the form *.python[23][0-9]+[.]dll
  int n = strlen(filename);
  if (n < 10) {
    FAIL;
  }

  char * p = filename + n - 1;
  while (*(p--) != 'n' && p > filename);
  p++;
  *major = *(p++) - '0';
  if (*major != 2 && *major != 3) {
    FAIL;
  }

  if (sscanf(p,"%d.dll", minor) == 1) {
    SUCCESS;
  }

  #elif defined PL_MACOS                                               /* MAC */
  char * ver_needle = strstr(filename, "3.");
  if (ver_needle == NULL) ver_needle = strstr(filename, "2.");
  if (ver_needle != NULL && sscanf(ver_needle, "%d.%d", major, minor) == 2) {
    SUCCESS;
  }  

  #endif

  FAIL;
} /* _get_version_from_filename */

#if defined PL_MACOS
static int
_find_version_in_binary(char * path) {
  log_d("Finding version in binary %s", path);

  int fd = open (path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary file %s", path);
    FAIL;
  }

  void        * binary_map  = MAP_FAILED;
  size_t        binary_size = 0;
  struct stat   s;

  with_resources;

  if (fstat(fd, &s) == -1) {
    log_ie("Cannot determine size of binary file");
    NOK;
  }

  binary_size = s.st_size;

  binary_map = mmap(0, binary_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (binary_map == MAP_FAILED) {
    log_ie("Cannot map binary file to memory");
    NOK;
  }

  for (char m = '3'; m >= '2'; --m) {
    char     needle[3]    = {0x00, m, '.'};
    size_t   current_size = binary_size;
    char   * current_pos  = binary_map;
    int      major, minor, patch;
    major = 0;
    retval = NOVERSION;
    while (TRUE) {
      char * p = memmem(current_pos, current_size, needle, sizeof(needle));
      if (!isvalid(p)) break;
      if (sscanf(++p, "%d.%d.%d", &major, &minor, &patch) == 3) break;
      current_size -= p - current_pos + sizeof(needle);
      current_pos = p + sizeof(needle);
    }

    if (major > 0) {
      retval = PYVERSION(major, minor, patch);
      break;
    }
  }

release:
  if (binary_map != MAP_FAILED) munmap(binary_map, binary_size);
  if (fd != -1) close(fd);

  released;
} /* _find_version_in_binary */
#endif


static int
_py_proc__infer_python_version(py_proc_t * self) {
  if (self == NULL || (self->bin_path == NULL && self->lib_path == NULL))
    FAIL;

  int major = 0, minor = 0, patch = 0;

  // Starting with Python 3.11 we can rely on the Py_Version symbol
  if (isvalid(self->symbols[DYNSYM_HEX_VERSION])) {
    unsigned long py_version = 0;
    
    if (fail(py_proc__memcpy(self, self->symbols[DYNSYM_HEX_VERSION], sizeof(py_version), &py_version))) {
      log_e("Failed to dereference remote Py_Version symbol");
      return NOVERSION;
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
  &&success(_get_version_from_filename(self->lib_path, &major, &minor, &patch))
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

  #if defined PL_MACOS
  if (major == 0) {
    // We still haven't found a Python version so we look at the binary
    // content for clues
    int version = NOVERSION;
    if (isvalid(self->bin_path) && (version = _find_version_in_binary(self->bin_path))) {
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
}


// ----------------------------------------------------------------------------
static int
_py_proc__check_interp_state(py_proc_t * self, void * raddr) {
  if (!isvalid(self))
    FAIL;

  V_DESC(self->py_v);

  PyInterpreterState is;
  PyThreadState      tstate_head;

  if (py_proc__get_type(self, raddr, is))
    return OUT_OF_BOUND;

  if (py_proc__get_type(self, V_FIELD(void *, is, py_is, o_tstate_head), tstate_head)) {
    log_t(
      "Cannot copy PyThreadState head at %p from PyInterpreterState instance",
      V_FIELD(void *, is, py_is, o_tstate_head)
    );
    FAIL;
  }

  log_t("PyThreadState head loaded @ %p", V_FIELD(void *, is, py_is, o_tstate_head));

  if (V_FIELD(void*, tstate_head, py_thread, o_interp) != raddr)
    FAIL;

  log_d(
    "Found possible interpreter state @ %p (offset %p).",
    raddr, raddr - self->map.heap.base
  );

  log_t(
    "PyInterpreterState loaded @ %p. Thread State head @ %p",
    raddr, V_FIELD(void *, is, py_is, o_tstate_head)
  );

  #if defined PL_LINUX
  raddr_t thread_raddr = {self->proc_ref, V_FIELD(void *, is, py_is, o_tstate_head)};
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

  // Try to determine the TID by reading the remote struct pthread structure.
  // We can then use this information to parse the appropriate procfs file and
  // determine the native thread's running state.
  while (isvalid(thread.raddr.addr)) {
    if (success(_infer_tid_field_offset(&thread)))
      SUCCESS;
    py_thread__next(&thread);
  }
  log_d("tid field offset not ready");
  FAIL;
  #endif

  SUCCESS;
}


#ifdef CHECK_HEAP
// ----------------------------------------------------------------------------
static int
_py_proc__is_heap_raddr(py_proc_t * self, void * raddr) {
  if (!isvalid(self) || !isvalid(raddr) || !isvalid(self->map.heap.base))
    return FALSE;

  return (
    raddr >= self->map.heap.base &&
    raddr < self->map.heap.base + self->map.heap.size
  );
}


// ----------------------------------------------------------------------------
static int
_py_proc__is_raddr_within_max_range(py_proc_t * self, void * raddr) {
  if (!isvalid(self) || !isvalid(raddr) || !isvalid(self->map.heap.base))
    return FALSE;

  return (raddr >= self->min_raddr && raddr < self->max_raddr);
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_heap(py_proc_t * self) {
  log_d("Scanning HEAP");
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
  if (fail(py_proc__memcpy(self, self->map.bss.base, self->map.bss.size, self->bss)))
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
  if (!isvalid(self))
    FAIL;

  V_DESC(self->py_v);
  
  void * interp_head_raddr;

  if (self->symbols[DYNSYM_RUNTIME] != NULL) {
    _PyRuntimeState py_runtime;
    if (py_proc__get_type(self, self->symbols[DYNSYM_RUNTIME], py_runtime)) {
      log_d(
        "Cannot copy _PyRuntimeState structure from remote address %p",
        self->symbols[DYNSYM_RUNTIME]
      );
      FAIL;
    }
    interp_head_raddr = V_FIELD(void *, py_runtime, py_runtime, o_interp_head);
    if (V_MAX(3, 8)) {
      self->gc_state_raddr = self->symbols[DYNSYM_RUNTIME] + py_v->py_runtime.o_gc;
      log_d("GC runtime state @ %p", self->gc_state_raddr);
    }
  }
  else if (self->symbols[DYNSYM_INTERP_HEAD] != NULL) {
    if (py_proc__get_type(self, self->symbols[DYNSYM_INTERP_HEAD], interp_head_raddr)) {
      log_d(
        "Cannot copy PyInterpreterState structure from remote address %p",
        self->symbols[DYNSYM_INTERP_HEAD]
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
static inline void *
_py_proc__get_current_thread_state_raddr(py_proc_t * self) {
  void * p_tstate_current;

  if (self->symbols[DYNSYM_RUNTIME] != NULL) {
    if (self->tstate_current_offset == 0 || py_proc__get_type(
      self,
      self->symbols[DYNSYM_RUNTIME] + self->tstate_current_offset,
      p_tstate_current
    )) return (void *) -1;
  }

  else if (self->symbols[DYNSYM_THREADSTATE_CURRENT] != NULL) {
    if (py_proc__get_type(self, self->symbols[DYNSYM_THREADSTATE_CURRENT], p_tstate_current))
      return (void *) -1;
  }

  else return (void *) -1;

  return p_tstate_current;
}


// ----------------------------------------------------------------------------
static int
_py_proc__find_interpreter_state(py_proc_t * self) {
  if (!isvalid(self))
    FAIL;

  V_DESC(self->py_v);
  
  PyThreadState   tstate_current;
  void          * tstate_current_raddr;

  // First try to de-reference interpreter head as the most reliable method
  if (_py_proc__deref_interp_head(self)) {
    log_d("Cannot dereference PyInterpreterState head from symbols");
    // If that fails try to get the current thread state (can be NULL during idle)
    tstate_current_raddr = _py_proc__get_current_thread_state_raddr(self);
    if (tstate_current_raddr == NULL || tstate_current_raddr == (void *) -1)
      // Idle or unable to dereference
      FAIL;
    else {
      if (fail(py_proc__get_type(self, tstate_current_raddr, tstate_current)))
        FAIL;

      if (fail(_py_proc__check_interp_state(
        self, V_FIELD(void*, tstate_current, py_thread, o_interp)
      ))) FAIL;

      self->is_raddr = V_FIELD(void*, tstate_current, py_thread, o_interp);
      log_d("Interpreter head de-referenced from current thread state symbol.");
    }
  } else {
    log_d("✨ Interpreter head de-referenced from symbols ✨ ");
  }

  SUCCESS;

  // 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
  //                 anymore, but _PyThreadState_Current.prev is.
  /* if (
      V_FIELD(void*, tstate_current, py_thread, o_thread_id) == 0 && \
      V_FIELD(void*, tstate_current, py_thread, o_prev)      != 0
    ) {
      self->symbols[DYNSYM_THREADSTATE_CURRENT] = V_FIELD(void*, tstate_current, py_thread, o_prev);
      return 1;
    } */
}
#endif  // DEREF_SYM


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  #ifdef DEBUG
  register int attempts = 0;
  #endif

  self->is_raddr = NULL;

  TIMER_RESET
  TIMER_START
    if (!py_proc__is_running(self)) {
      set_error(EPROCNPID);
      FAIL;
    }

    #ifdef DEBUG
    attempts++;
    #endif

    #ifdef DEREF_SYM
    if (fail(_py_proc__find_interpreter_state(self))) {
    #endif
      if (is_fatal(austin_errno)) {
        log_d(
          "Terminatig _py_proc__wait_for_interp_state loop because of fatal error code %d",
          austin_errno
        );
        FAIL;
      }
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
  TIMER_SET(10)
  TIMER_START
    switch (_py_proc__scan_heap(self)) {
    case 0:
      SUCCESS;

    case OUT_OF_BOUND:
      TIMER_STOP
    }
  TIMER_END
  #endif

  set_error(EPROCISTIMEOUT);
  FAIL;
} // _py_proc__wait_for_interp_state


// ----------------------------------------------------------------------------
static int
_py_proc__run(py_proc_t * self) {
  austin_errno = EOK;

  int try_once = self->child;
  #ifdef DEBUG
  if (try_once == FALSE)
    log_d("Start up timeout: %d ms", pargs.timeout / 1000);
  else
    log_d("Single attempt to attach to process %d", self->pid);
  #endif

  TIMER_RESET
  TIMER_START
    if (!py_proc__is_running(self)) {
      set_error(EPROCNPID);
      FAIL;
    }

    sfree(self->bin_path);
    sfree(self->lib_path);
    self->sym_loaded = 0;

    if (success(_py_proc__init(self)))
      break;

    if (is_fatal(austin_errno)) {
      log_d("Terminatig _py_proc__run loop because of fatal error code %d", austin_errno);
      FAIL;
    }

    log_d("Process is not ready");

    if (try_once)
      TIMER_STOP
  TIMER_END
  log_d("_py_proc__init timer loop terminated");

  if (self->bin_path == NULL && self->lib_path == NULL) {
    if (try_once)
      log_d("Cannot attach to process %d with a single attempt.", self->pid);
    set_error(EPROC);
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
    self->symbols[DYNSYM_THREADSTATE_CURRENT] == NULL &&
    self->symbols[DYNSYM_RUNTIME]             == NULL &&
    self->symbols[DYNSYM_INTERP_HEAD]         == NULL &&
    self->gc_state_raddr                      == NULL
  )
    log_w("No remote symbol references have been set.");
  #endif

  #ifdef DEBUG
  if (self->bin_path != NULL) log_d("Python binary:  %s", self->bin_path);
  if (self->lib_path != NULL) log_d("Python library: %s", self->lib_path);
  log_d("Maximal VM address space: %p-%p", self->min_raddr, self->max_raddr);
  #endif

  // Determine and set version
  if (fail(_py_proc__infer_python_version(self))) {
    set_error(ENOVERSION);
    FAIL;
  }

  if (_py_proc__wait_for_interp_state(self))
    FAIL;

  self->timestamp = gettime();

  #ifdef NATIVE
  self->unwind.as = unw_create_addr_space(&_UPT_accessors, 0);
  #endif
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
  py_proc->min_raddr = (void *) -1;
  py_proc->gc_state_raddr = NULL;

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
    #if defined PL_WIN
    if (fail(_py_proc__try_child_proc(self))) {
    #endif
      if (austin_errno == EPROCNPID) {
        set_error(EPROCATTACH);
      }
      else {
        log_ie("Cannot attach to running process.");
      }
      FAIL;
    #if defined PL_WIN
    }
    #endif
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
    #if defined PL_WIN
    if (fail(_py_proc__try_child_proc(self))) {
    #endif
      if (austin_errno == EPROCNPID)
        set_error(EPROCFORK);
      log_ie("Cannot start new process");
      FAIL;
    #if defined PL_WIN
    }
    #endif
  }

  #ifdef NATIVE
  self->timestamp = gettime();
  #endif

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
  if (self->symbols[DYNSYM_RUNTIME] == NULL)
    FAIL;

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
        FAIL;
      }
      log_t("ptrace: thread %d resumed", py_thread.tid);
      if (fail(py_thread__set_interrupted(&py_thread, FALSE))) {
        log_ie("Failed to mark thread as interrupted");
        FAIL;
      }
    }
  } while (success(py_thread__next(&py_thread)));

  SUCCESS;
}
#endif


// ----------------------------------------------------------------------------
int
py_proc__sample(py_proc_t * self) {
  ctime_t   time_delta     = gettime() - self->timestamp;  // Time delta since last sample.
  ssize_t   mem_delta      = 0;
  void    * current_thread = NULL;

  V_DESC(self->py_v);

  PyInterpreterState is;
  if (fail(py_proc__get_type(self, self->is_raddr, is))) {
    log_ie("Failed to get interpreter state while sampling");
    FAIL;
  }

  void * tstate_head = V_FIELD(void *, is, py_is, o_tstate_head);
  if (isvalid(tstate_head)) {
    raddr_t raddr = { .pref = self->proc_ref, .addr = tstate_head };
    py_thread_t py_thread;

    #ifdef NATIVE
    if (fail(_py_proc__interrupt_threads(self, &raddr))) {
      log_ie("Failed to interrupt threads");
      FAIL;
    }
    time_delta = gettime() - self->timestamp;
    #endif

    if (fail(py_thread__fill_from_raddr(&py_thread, &raddr, self))) {
      log_ie("Failed to fill thread from raddr while sampling");
      if (is_fatal(austin_errno)) {
        FAIL;
      }
      SUCCESS;
    }

    if (pargs.memory) {
      // Use the current thread to determine which thread is manipulating memory
      current_thread = _py_proc__get_current_thread_state_raddr(self);
    }

    do {
      if (pargs.memory) {
        mem_delta = 0;
        if (self->symbols[DYNSYM_RUNTIME] != NULL && current_thread == (void *) -1) {
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
        time_delta,
        mem_delta
      );
    } while (success(py_thread__next(&py_thread)));
    #ifdef NATIVE
    self->timestamp = gettime();
    if (fail(_py_proc__resume_threads(self, &raddr))) {
      log_ie("Failed to resume threads");
      FAIL;
    }
    #endif

  }

  #ifndef NATIVE
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
void
py_proc__terminate(py_proc_t * self) {
  if (self->pid) {
    log_d("Terminating process %u", self->pid);
    #if defined PL_UNIX
    kill(self->pid, SIGTERM);
    #else
    TerminateProcess(self->proc_ref, 42);
    #endif
  }
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

  sfree(self->bin_path);
  sfree(self->lib_path);
  sfree(self->bss);
  sfree(self->extra);

  lru_cache__destroy(self->string_cache);
  lru_cache__destroy(self->frame_cache);

  free(self);
}
