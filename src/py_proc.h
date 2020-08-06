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

#include "stats.h"


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

typedef struct {
  pid_t           pid;

  char          * bin_path;
  char          * lib_path;

  proc_vm_map_t   map;
  void          * min_raddr;
  void          * max_raddr;

  void          * bss;  // local copy of the remote bss section

  int             sym_loaded;
  int             version;

  // Symbols from .dynsym
  void          * tstate_curr_raddr;
  void          * py_runtime_raddr;
  void          * interp_head_raddr;

  void          * is_raddr;

  // Temporal profiling support
  ctime_t         timestamp;

  // Memory profiling support
  ssize_t         last_resident_memory;

  // Offset of the tstate_current field within the _PyRuntimeState structure
  unsigned int    tstate_current_offset;

  // Platform-dependent fields
  proc_extra_info * extra;
} py_proc_t;


/**
 * Create a new process object. Use it to start the process that needs to be
 * sampled from austin.
 *
 * @return  a pointer to the newly created py_proc_t object.
 */
py_proc_t *
py_proc_new(void);


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
 * @param int          TRUE if this is a child process, FALSE otherwise.
 *
 * @return 0 on success.
 */
int
py_proc__attach(py_proc_t *, pid_t, int);


/**
 * Get the remote address of the PyInterpreterState instance.
 *
 * @param  py_proc_t * the process object.
 *
 * @return the remote address of the PyInterpreterState instance.
 */
void *
py_proc__get_istate_raddr(py_proc_t *);


/**
 * Get the remote address of the current PyThreadState instance.
 *
 * @param  py_proc_t * the process object.
 *
 * @return the remote address of the current PyThreadState instance. If no
 *         thread is currently running then this returns NULL. If an error
 *         occurred, the return value is (void *) -1.
 */
void *
py_proc__get_current_thread_state_raddr(py_proc_t *);


/**
 * Copy a chunk of memory from the process.
 *
 * @param py_proc_t * the process object.
 * @param void *      the remote address.
 * @param ssize_t     the number of bytes to read.
 * @param void *      the local buffer, of size at least matching the number
 *                    of bytes to read.
 *
 * @return 0 on success.
 */
int
py_proc__memcpy(py_proc_t *, void *, ssize_t, void *);


/**
 * Wait for the process to terminate.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__wait(py_proc_t *);


/**
 * Find the offset of the pointer to the current thread structure from the
 * beginning of the _PyRuntimeState structure (Python 3.7+ only).
 *
 * @param py_proc_t * the process object.
 * @param void      * the remote address of the thread to use for comparison.
 *
 * @return 0 on success, 1 otherwise.
 */
int
py_proc__find_current_thread_offset(py_proc_t * self, void * thread_raddr);


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
 * Get the memory size delta since last call.
 *
 * @param py_proc_t * the process object.
 *
 * @return the computed memory usage delta in bytes.
 */
ssize_t
py_proc__get_memory_delta(py_proc_t *);


/**
 * Sample the frame stack of each thread of the given Python process.
 *
 * @param  py_proc_t *  self.

 * @return 0 if the sampling succeded; 1 otherwise.
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
 * Terminate the process.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__terminate(py_proc_t *);


void
py_proc__destroy(py_proc_t *);

#endif // PY_PROC_H
