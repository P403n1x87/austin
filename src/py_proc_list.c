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

#if defined PL_LINUX
#include <dirent.h>
#elif defined PL_MACOS
#include <libproc.h>
#define PID_MAX 99999  // From sys/proc_internal.h
#elif defined PL_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hints.h"
#include "logging.h"
#include "timing.h"

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
} /* _py_proc_list__add */


// ----------------------------------------------------------------------------
static int
_py_proc_list__has_pid(py_proc_list_t * self, pid_t pid) {
  return self->index[pid] != NULL;
} /* _py_proc_list__has_pid */


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
} /* _py_proc_list__remove */


// ----------------------------------------------------------------------------
py_proc_list_t *
py_proc_list_new(py_proc_t * parent_py_proc) {
  py_proc_list_t * list = (py_proc_list_t *) calloc(1, sizeof(py_proc_list_t));
  if (list == NULL)
    return NULL;

  #if defined PL_LINUX                                               /* LINUX */
  FILE * pid_max_file = fopen("/proc/sys/kernel/pid_max", "rb");
  if (pid_max_file == NULL)
    return NULL;

  int has_pid_max = (fscanf(pid_max_file, "%d", &(list->pids)) == 1);
  fclose(pid_max_file);
  if (!has_pid_max)
    return NULL;

  #elif defined PL_MACOS                                             /* MACOS */
  list->pids = PID_MAX;

  #elif defined PL_WIN                                                 /* WIN */
  list->pids = (1 << 22);  // 4M.  WARNING: This could potentially be violated!

  #endif

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
} /* py_proc_list_new */


// ----------------------------------------------------------------------------
void
py_proc_list__add_proc_children(py_proc_list_t * self, pid_t ppid) {
  for (register pid_t pid = 0; pid <= self->max_pid; pid++) {
    if (self->pid_table[pid] == ppid && !_py_proc_list__has_pid(self, pid)) {
      py_proc_t * child_proc = py_proc_new();
      if (child_proc == NULL)
        continue;

      if (py_proc__attach(child_proc, pid, TRUE)) {
        py_proc__destroy(child_proc);
        continue;
      }

      _py_proc_list__add(self, child_proc);
      py_proc__log_version(child_proc, FALSE);
      py_proc_list__add_proc_children(self, pid);
    }
  }
} /* py_proc_list__add_proc_children */


// ----------------------------------------------------------------------------
int
py_proc_list__is_empty(py_proc_list_t * self) {
  return !isvalid(self->first);
} /* py_proc_list__is_empty */


// ----------------------------------------------------------------------------
void
py_proc_list__sample(py_proc_list_t * self) {
  log_t("Sampling from process list");

  for (py_proc_item_t * item = self->first; item != NULL; item = item->next) {
    log_t("Sampling process with PID %d", item->py_proc->pid);
    stopwatch_start();
    py_proc__sample(item->py_proc);  // Fail silently
    stopwatch_duration();
  }
} /* py_proc_list__sample */


// ----------------------------------------------------------------------------
int
py_proc_list__size(py_proc_list_t * self) {
  return self->count;
}


// ----------------------------------------------------------------------------
void
py_proc_list__update(py_proc_list_t * self) {
  ctime_t now = gettime();
  if (now - self->timestamp < UPDATE_INTERVAL)
    return;  // Do not update too frequently as this is an expensive operation.

  memset(self->pid_table, 0, self->pids * sizeof(pid_t));
  self->max_pid = 0;

  // Update PID table
  #if defined PL_LINUX                                               /* LINUX */
  char stat_path[32];
  char buffer[1024];
  struct dirent *ent;

  DIR * proc_dir = opendir("/proc");
  if (proc_dir == NULL)
    goto finally;

  for (;;) {
    // This code is inspired by the ps util
    ent = readdir(proc_dir);
    if (!ent || !ent->d_name) break;
    if ((*ent->d_name <= '0') || (*ent->d_name > '9')) continue;

    unsigned long pid = strtoul(ent->d_name, NULL, 10);
    sprintf(stat_path, "/proc/%ld/stat", pid);

    FILE * stat_file = fopen(stat_path, "rb");
    if (stat_file == NULL)
      continue;

    char * line = NULL;
    size_t n;
    if (getline(&line, &n, stat_file) == 0) {
      log_w("Failed to read stat file for process %d", pid);
      sfree(line);
      continue;
    }
    char * stat = strchr(line, ')') + 2;
    if (stat[0] == ' ') stat++;
    if (sscanf(
      stat, "%c %d",
      (char *) buffer, &(self->pid_table[pid])
    ) != 2) {
      log_w("Failed to parse stat file for process %d", pid);
      sfree(line);
      continue;
    }
    sfree(line);

    if (pid > self->max_pid)
      self->max_pid = pid;

    fclose(stat_file);
  }

  closedir(proc_dir);

  #elif defined PL_MACOS                                             /* MACOS */
  int pid_list[PID_MAX];

  int n_pids = proc_listallpids(NULL, 0);
  if (!n_pids || proc_listallpids(pid_list, sizeof(pid_list)) == -1)
    goto finally;

  for (register int i = 0; i < n_pids; i++) {
    struct proc_bsdinfo proc;

    if (proc_pidinfo(pid_list[i], PROC_PIDTBSDINFO, 0, &proc, PROC_PIDTBSDINFO_SIZE) == -1)
      continue;

    self->pid_table[pid_list[i]] = proc.pbi_ppid;
    if (pid_list[i] > self->max_pid)
      self->max_pid = pid_list[i];
  }

  #elif defined PL_WIN                                                 /* WIN */
  HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE)
    goto finally;

  PROCESSENTRY32 pe = { 0 };
  pe.dwSize = sizeof(PROCESSENTRY32);

  if (Process32First(h, &pe)) {
    do {
      self->pid_table[pe.th32ProcessID] = pe.th32ParentProcessID;

      if (pe.th32ProcessID > self->max_pid)
        self->max_pid = pe.th32ProcessID;
    } while (Process32Next(h, &pe));
  }

  CloseHandle(h);
  #endif

  log_t("PID table populated");

  // Attach to new PIDs.
  for (py_proc_item_t * item = self->first; item != NULL; /* item = item->next */) {
    if (py_proc__is_running(item->py_proc)) {
      py_proc_list__add_proc_children(self, item->py_proc->pid);
      item = item->next;
    }
    else {
      log_d("Process %d no longer running", item->py_proc->pid);
      py_proc__wait(item->py_proc);

      py_proc_item_t * next = item->next;
      _py_proc_list__remove(self, item);
      item = next;
    }
  }

finally:
  self->timestamp = now;
} /* py_proc_list__update */


// ----------------------------------------------------------------------------
void
py_proc_list__wait(py_proc_list_t * self) {
  log_d("Waiting for child processes to terminate");

  for (py_proc_item_t * item = self->first; item != NULL; item = item->next)
    py_proc__wait(item->py_proc);
} /* py_proc_list__wait */


// ----------------------------------------------------------------------------
void
py_proc_list__destroy(py_proc_list_t * self) {
  // Remove all items first
  while (self->first)
    _py_proc_list__remove(self, self->first);

  sfree(self->index);
  sfree(self->pid_table);
  free(self);
} /* py_proc_list__destroy */
