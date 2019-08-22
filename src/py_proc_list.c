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

#include "platform.h"

#ifdef PL_LINUX
#include <dirent.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#include "py_proc_list.h"


#define UPDATE_INTERVAL           100000  // 0.1s


// ----------------------------------------------------------------------------
static void
_py_proc_list__add(py_proc_list_t * self, py_proc_t * py_proc) {
  py_proc_item_t * item = (py_proc_item_t *) malloc(sizeof(py_proc_item_t));
  if (item == NULL)
    return;

  // Insert at the beginning of the list
  item->py_proc = py_proc;

  item->next = self->first;
  item->prev = NULL;

  if (self->first)
    self->first->prev = item;

  self->first = item;

  // Update index table.
  self->index[py_proc->pid] = py_proc;

  self->count++;

  log_d("Added process with PID %d (total number of processes: %d)", py_proc->pid, self->count);
}


// ----------------------------------------------------------------------------
static int
_py_proc_list__has_pid(py_proc_list_t * self, pid_t pid) {
  return self->index[pid] != NULL;
}


// ----------------------------------------------------------------------------
static void
_py_proc_list__remove(py_proc_list_t * self, py_proc_item_t * item) {
  #ifdef DEBUG
  pid_t pid = item->py_proc->pid;
  #endif

  self->index[item->py_proc->pid] = NULL;

  if (item == self->first)
    self->first = item->next;

  if (item->next)
    item->next->prev = item->prev;

  if (item->prev)
    item->prev->next = item->next;

  py_proc__destroy(item->py_proc);
  free(item);

  self->count--;

  log_d("Removed process with PID %d. Items left: %d", pid, self->count);
}


// ----------------------------------------------------------------------------
py_proc_list_t *
py_proc_list_new(py_proc_t * parent_py_proc) {
  py_proc_list_t * list = (py_proc_list_t *) calloc(1, sizeof(py_proc_list_t));
  if (list == NULL)
    return NULL;

  FILE * pid_max_file = fopen("/proc/sys/kernel/pid_max", "rb");
  if (pid_max_file == NULL)
    return NULL;

  int has_pid_max = (fscanf(pid_max_file, "%d", &(list->pids)) == 1);
  fclose(pid_max_file);
  if (!has_pid_max)
    return NULL;

  log_t("Maximum number of PIDs: %d", list->pids);

  list->index = (py_proc_t **) calloc(list->pids, sizeof(py_proc_t *));
  if (list->index == NULL)
    return NULL;

  list->pid_table = (pid_t *) calloc(list->pids, sizeof(pid_t));
  if (list->pid_table == NULL) {
    free(list->index);
    return NULL;
  }

  // Add the parent process to the list.
  _py_proc_list__add(list, parent_py_proc);

  return list;
}


// ----------------------------------------------------------------------------
void
py_proc_list__add_proc_children(py_proc_list_t * self, pid_t ppid) {
  for (register pid_t pid = 0; pid < self->max_pid; pid++) {
    if (self->pid_table[pid] == ppid && !_py_proc_list__has_pid(self, pid)) {
      py_proc_t * child_proc = py_proc_new();
      if (child_proc == NULL)
        continue;

      if (py_proc__attach(child_proc, pid)) {
        py_proc__destroy(child_proc);
        continue;
      }

      _py_proc_list__add(self, child_proc);
      py_proc_list__add_proc_children(self, pid);
    }
  }
}


// ----------------------------------------------------------------------------
int
py_proc_list__is_empty(py_proc_list_t * self) {
  return self->first == NULL;
}


// ----------------------------------------------------------------------------
void
py_proc_list__sample(py_proc_list_t * self) {
  log_t("Sampling from process list");

  for (py_proc_item_t * item = self->first; item != NULL; item = item->next) {
    log_t("Sampling process with PID %d", item->py_proc->pid);
    if (py_proc__is_running(item->py_proc))
      py_proc__sample(item->py_proc);
  }
}


// ----------------------------------------------------------------------------
void
py_proc_list__update(py_proc_list_t * self) {
  ctime_t now = gettime();
  if (now - self->timestamp < UPDATE_INTERVAL)
    return;  // Do not update too frequently as this is an expensive operation.

  #ifdef PL_LINUX                                                    /* LINUX */
  char stat_path[32];
  char buffer[1024];
  struct dirent *ent;

  memset(self->pid_table, 0, self->pids * sizeof(pid_t));
  self->max_pid = 0;

  DIR * proc_dir = opendir("/proc");
  {
    for (;;) {
      // This code is inspired by the ps util
      ent = readdir(proc_dir);
      if (!ent || !ent->d_name) break;
      if ((*ent->d_name <= '0') || (*ent->d_name > '9')) continue;

      unsigned long pid = strtoul(ent->d_name, NULL, 10);
      sprintf(stat_path, "/proc/%ld/stat", pid);

      FILE * stat_file = fopen(stat_path, "rb");
      if (stat_file != NULL) {
        fscanf(
          stat_file, "%d %s %c %d",
          (int *) buffer, buffer, (char *) buffer, &(self->pid_table[pid])
        );

        if (pid > self->max_pid)
          self->max_pid = pid;

        fclose(stat_file);
      }
    }
  }
  closedir(proc_dir);
  #endif

  log_t("PID table populated");

  // Attach to new PIDs.
  for (py_proc_item_t * item = self->first; item != NULL; /* item = item->next */) {
    if (py_proc__is_running(item->py_proc)) {
      py_proc_list__add_proc_children(self, item->py_proc->pid);
      item = item->next;
    }
    else {
      py_proc_item_t * next = item->next;
      _py_proc_list__remove(self, item);
      item = next;
    }
  }

  self->timestamp = now;
}


// ----------------------------------------------------------------------------
void
py_proc_list__wait(py_proc_list_t * self) {
  log_d("Waiting for child processes to terminate");

  for (py_proc_item_t * item = self->first; item != NULL; item = item->next) {
    if (py_proc__is_running(item->py_proc))
      py_proc__wait(item->py_proc);
  }
}


// ----------------------------------------------------------------------------
void
py_proc_list__destroy(py_proc_list_t * self) {
  // Remove all items first
  while (self->first)
    _py_proc_list__remove(self, self->first);

  if (self->index != NULL)
    free(self->index);

  if (self->pid_table != NULL)
    free(self->pid_table);

  free(self);
}
