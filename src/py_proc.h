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

#pragma once

#include <stdbool.h>
#include <sys/types.h>

#ifdef NATIVE
#include "cache.h"
#include "linux/vm-range-tree.h"
#include <libunwind-ptrace.h>
#endif

#include "cache.h"
#include "platform.h"
#include "python/symbols.h"
#include "stats.h"
#include "version.h"

typedef struct {
    raddr_t base;
    ssize_t size;
} proc_vm_map_block_t;

typedef struct {
    proc_vm_map_block_t bss;
    proc_vm_map_block_t exe;
    proc_vm_map_block_t dynsym;
    proc_vm_map_block_t rodata;
    proc_vm_map_block_t runtime; // Added in Python 3.11
} proc_vm_map_t;

typedef struct {
    size_t base_offset;
    size_t size;
    void*  data;
} com_t;

typedef struct _proc_extra_info proc_extra_info; // Forward declaration.

typedef struct {
    pid_t      pid;
    proc_ref_t ref;
    bool       child;

    char* bin_path;
    char* lib_path;

    proc_vm_map_t map;

    int       sym_loaded;
    python_v* py_v;

    raddr_t symbols[DYNSYM_COUNT]; // Binary symbols

    raddr_t gc_state_raddr;

    raddr_t istate_raddr;

    lru_cache_t* frame_cache;
    lru_cache_t* string_cache;
    lru_cache_t* code_cache;
    lru_cache_t* interpreter_state_cache;

    // Temporal profiling support
    microseconds_t timestamp;

    // Memory profiling support
    ssize_t last_resident_memory;

    // Offset of the tstate_current field within the _PyRuntimeState structure
    unsigned int tstate_current_offset;

#ifdef NATIVE
    struct _puw {
        unw_addr_space_t as;
    } unwind;
    vm_range_tree_t* maps_tree;
    hash_table_t*    base_table;
#endif

    com_t interpreter_state_com;

    // Platform-dependent fields
    proc_extra_info* extra;
} py_proc_t;

/**
 * Create a new process object. Use it to start the process that needs to be
 * sampled from austin.
 *
 * @param child  whether this is a child process.
 *
 * @return a pointer to the newly created py_proc_t object.
 */
py_proc_t*
py_proc_new(bool child);

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
py_proc__start(py_proc_t*, const char*, char**);

/**
 * Attach the process with the given PID
 *
 * @param py_proc_t *  the process object.
 * @param pid_t        the PID of the process to attach.
 *
 * @return 0 on success.
 */
int
py_proc__attach(py_proc_t*, pid_t);

/**
 * Wait for the process to terminate.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__wait(py_proc_t*);

/**
 * Check if the process is still running.
 *
 * @param py_proc_t * the process object.
 *
 * @return true if the process is still running, false otherwise.
 */
bool
py_proc__is_running(py_proc_t*);

/**
 * Check if the process is a Python process.
 *
 * @param py_proc_t * the process object.
 *
 * @return true if the process is a Python process, false otherwise.
 */
bool
py_proc__is_python(py_proc_t*);

/**
 * Check whether the GC is collecting for the given process.
 *
 * NOTE: This method makes sense only for Python>=3.7.
 *
 * @param py_proc_t * the process object.
 *
 * @return true if the GC is collecting, false otherwise.
 *
 */
bool
py_proc__is_gc_collecting(py_proc_t*);

/**
 * Sample the frame stack of each thread of the given Python process.
 *
 * @param  py_proc_t *  self.

 * @return 0 if the sampling succeeded; 1 otherwise.
 */
int
py_proc__sample(py_proc_t*);

/**
 * Initialise the process. Useful after an exec.
 *
 * @param  py_proc_t * self
 *
 * @return 0 on success; 1 otherwise
 */
int
py_proc__init(py_proc_t*);

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
 * Make a local copy of a remote structure.
 *
 * @param self  the process object.
 * @param type  the type of the structure.
 * @param raddr the remote address of the structure.
 * @param dest  the destination address.
 *
 * @return 0 on success.
 */
#define py_proc__copy_v(self, type, raddr, dest) (py_proc__memcpy(self, raddr, py_v->py_##type.size, dest))

/**
 * Copy a field from a versioned Python data structure.
 * @param self   the process object.
 * @param type   the versioned Python type (e.g. runtime).
 * @param field  the field name (e.g. interp_head).
 * @param raddr  the remote address of the versioned Python data structure.
 * @param dst    the destination variable.

 * @return        zero on success, otherwise non-zero.
 */
#define py_proc__copy_field_v(self, type, field, raddr, dst) copy_field_v(self->ref, type, field, raddr, dst)

/**
 * Log the Python interpreter version
 * @param self  the process object.
 * @param bool  whether the process is the parent process.
 */
void
py_proc__log_version(py_proc_t*, bool);

/**
 * Send a signal to the process.
 *
 * @param py_proc_t * the process object.
 * @param int         the signal to send to the process.
 */
void
py_proc__signal(py_proc_t*, int);

/**
 * Terminate the process.
 *
 * @param py_proc_t * the process object.
 */
void
py_proc__terminate(py_proc_t*);

void
py_proc__destroy(py_proc_t*);
