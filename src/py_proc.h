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

#ifndef PY_PROC_H
#define PY_PROC_H


#include <sys/types.h>

#ifdef NATIVE
#include <libunwind-ptrace.h>
#include "linux/vm-range-tree.h"
#include "cache.h"
#endif

#include "python/symbols.h"
#include "cache.h"
#include "heap.h"
#include "platform.h"
#include "stats.h"
#include "version.h"


typedef struct {
  void    * base;
  ssize_t   size;
} proc_vm_map_block_t;


typedef struct {
  proc_vm_map_block_t bss;
  proc_vm_map_block_t heap;
  proc_vm_map_block_t elf;
  proc_vm_map_block_t dynsym;
  proc_vm_map_block_t rodata;
} proc_vm_map_t;

typedef struct _proc_extra_info proc_extra_info;  // Forward declaration.

typedef struct _py_proc_t {
  pid_t           pid;
  proc_ref_t      proc_ref;
  int             child;

  char          * bin_path;
  char          * lib_path;

  proc_vm_map_t   map;
  void          * min_raddr;
  void          * max_raddr;

  void          * bss;  // local copy of the remote bss section

  int             sym_loaded;
  python_v      * py_v;

  void          * symbols[DYNSYM_COUNT];  // Binary symbols

  void          * gc_state_raddr;

  void          * is_raddr;

  lru_cache_t   * frame_cache;
  lru_cache_t   * string_cache;

  // Temporal profiling support
  ctime_t         timestamp;

  // Memory profiling support
  ssize_t         last_resident_memory;

  // Offset of the tstate_current field within the _PyRuntimeState structure
  unsigned int    tstate_current_offset;

  // Frame objects VM ranges
  _mem_block_t    frames;
  _mem_block_t    frames_heap;

  #ifdef NATIVE
  struct _puw {
    unw_addr_space_t as;
  }                 unwind;
  vm_range_tree_t * maps_tree;
  hash_table_t    * base_table;
  #endif

  // Platform-dependent fields
  proc_extra_info * extra;
} py_proc_t;


/**
 * Create a new process object. Use it to start the process that needs to be
 * sampled from austin.
 * 
 * @param child  whether this is a child process.
 *
 * @return a pointer to the newly created py_proc_t object.
 */
py_proc_t *
py_proc_new(int child);


/**
 * Start the process
 *
 * @param py_proc_t *  the process object.
 * @param const char * the command to execute.
 * @param char **      the command line arguments to pass to the command.
 *
 * @return 0 on success.
 */
int
py_proc__start(py_proc_t *, const char *, char **);


/**
 * Attach the process with the given PID
 *
 * @param py_proc_t *  the process object.
 * @param pid_t        the PID of the process to attach.
 *
 * @return 0 on success.
 */
int
py_proc__attach(py_proc_t *, pid_t);


/**
 * Wait for the process to terminate.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__wait(py_proc_t *);


/**
 * Check if the process is still running.
 *
 * @param py_proc_t * the process object.
 *
 * @return 1 if the process is still running, 0 otherwise.
 */
int
py_proc__is_running(py_proc_t *);


/**
 * Check if the process is a Python process.
 *
 * @param py_proc_t * the process object.
 *
 * @return 1 if the process is a Python process, 0 otherwise.
 */
int
py_proc__is_python(py_proc_t *);


/**
 * Check whether the GC is collecting for the given process.
 * 
 * NOTE: This method makes sense only for Python>=3.7.
 * 
 * @param py_proc_t * the process object.
 * 
 * @return TRUE if the GC is collecting, FALSE otherwise.
 * 
 */
int
py_proc__is_gc_collecting(py_proc_t *);


/**
 * Sample the frame stack of each thread of the given Python process.
 *
 * @param  py_proc_t *  self.

 * @return 0 if the sampling succeeded; 1 otherwise.
 */
int
py_proc__sample(py_proc_t *);


/**
 * Get a datatype from the process
 *
 * @param self  the process object.
 * @param raddr the remote address of the datatype.
 * @param dt    the datatype as a local variable.
 *
 * @return 0 on success.
 */
#define py_proc__get_type(self, raddr, dt) (py_proc__memcpy(self, raddr, sizeof(dt), &dt))

/**
 * Log the Python interpreter version
 * @param self  the process object.
 * @param int   whether the process is the parent process.
 */
void
py_proc__log_version(py_proc_t *, int);


/**
 * Terminate the process.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__terminate(py_proc_t *);


void
py_proc__destroy(py_proc_t *);

#ifdef LIBAUSTIN
#include "frame.h"

/**
 * Sample the frame stack of each thread of the given Python process.
 *
 * @param  py_proc_t *                        self.
 * @param  (void*) (*callback)(pid_t, pid_t)  the callback function to cal
 *                                            when a thread stack is available.

 * @return 0 if the sampling succeeded; 1 otherwise.
 */
int
py_proc__sample_cb(py_proc_t *, void (*)(pid_t, pid_t));


int
py_proc__sample_thread(py_proc_t *, pid_t);


frame_t *
py_proc__read_frame(py_proc_t *, void *);

#endif

#endif // PY_PROC_H
