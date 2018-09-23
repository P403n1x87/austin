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


#include "python36.h"


typedef struct {
  void    * base;
  ssize_t   size;
} proc_vm_map_block_t;


typedef struct {
  proc_vm_map_block_t elf;
  proc_vm_map_block_t dynsym;
  proc_vm_map_block_t rodata;
} proc_vm_map_t;


typedef struct {
  pid_t           pid;

  char          * bin_path;

  proc_vm_map_t   map;

  // Local copy of the dynsym section
  int             sym_loaded;
  int             version;
  void          * tstate_curr_raddr;

  void          * is_raddr;
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
 * Get the remote address of the PyInterpreterState instance.
 *
 * @param  py_proc_t * the process object.
 *
 * @return the remote address of the PyInterpreterState instance.
 */
void *
py_proc__get_istate_raddr(py_proc_t *);


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
 * Check if the process is still running
 *
 * @param py_proc_t * the process object.
 *
 * @return 1 if the process is still running, 0 otherwise.
 */
int
py_proc__is_running(py_proc_t *);


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


void
py_proc__destroy(py_proc_t *);

#endif // PY_PROC_H
