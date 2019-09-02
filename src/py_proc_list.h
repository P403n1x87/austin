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

#ifndef PY_PROC_LIST_H
#define PY_PROC_LIST_H


#include "py_proc.h"


typedef struct _py_proc_item {
  py_proc_t            * py_proc;
  struct _py_proc_item * next;
  struct _py_proc_item * prev;
} py_proc_item_t;


typedef struct {
  int              count;      // Number of entries in the list
  py_proc_item_t * first;      // First item in the list
  py_proc_t     ** index;      // Index of PIDs in the list
  pid_t          * pid_table;  // Table of pids with their parents
  pid_t            max_pid;    // Highest seen PID in the index
  int              pids;       // Maximum number of PIDs in the index
  ctime_t          timestamp;  // Timestamp of the last update
} py_proc_list_t;


/**
 * Constructor.
 *
 * This list manages the children of the given parent process.
 *
 * @param  py_proc_t  the parent process.
 */

py_proc_list_t *
py_proc_list_new(py_proc_t *);


/**
 * Check if the list is empty.
 *
 * @param  py_proc_list_t  the list.
 *
 * @return 1 if empty; 0 otherwise.
 */
int
py_proc_list__is_empty(py_proc_list_t *);


/**
 * Add the the children of the given process to the list.
 *
 * @param  py_proc_list_t  the list.
 * @param  pid_t           the PID of the parent process.
 */
void
py_proc_list__add_proc_children(py_proc_list_t *, pid_t);


/**
 * Sample from all the processes in the list.
 *
 * @param  py_proc_list_t  the list.
 */
void
py_proc_list__sample(py_proc_list_t *);


/**
 * Update the list.
 *
 * Refreshes the internal PID table and adds any new children of the currently
 * running processes in the list. Old processes that are not running anymore
 * are removed.
 *
 * This method is quite expensive so it is executed no more frequently than
 * once every 0.1s.
 *
 * @param  py_proc_list_t  the list.
 */

void
py_proc_list__update(py_proc_list_t *);


/**
 * Wait for all the processes in the list to terminate.
 *
 * @param  py_proc_list_t  the list.
 */

void
py_proc_list__wait(py_proc_list_t *);


/**
 * Destroy the list from memory.
 *
 * @param  py_proc_list_t  the list.
 */
void
py_proc_list__destroy(py_proc_list_t *);


#endif
