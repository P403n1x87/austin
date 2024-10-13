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

#include "linux/common.h"
#elif defined PL_MACOS
#include <libproc.h>
#elif defined PL_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hints.h"
#include "logging.h"
#include "resources.h"
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
  lookup__set(self->py_proc_for_pid, py_proc->pid, py_proc);

  self->count++;

  log_d("Added process with PID %d (total number of processes: %d)", py_proc->pid, self->count);
} /* _py_proc_list__add */


// ----------------------------------------------------------------------------
static int
_py_proc_list__has_pid(py_proc_list_t * self, pid_t pid) {
  return isvalid(lookup__get(self->py_proc_for_pid, pid));
} /* _py_proc_list__has_pid */


// ----------------------------------------------------------------------------
static void
_py_proc_list__remove(py_proc_list_t * self, py_proc_item_t * item) {
  #ifdef DEBUG
  pid_t pid = item->py_proc->pid;
  #endif

  lookup__del(self->py_proc_for_pid, item->py_proc->pid);

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
  if (!isvalid(list))
    return NULL;

  log_t("Maximum number of PIDs: %d", list->pids);

  list->py_proc_for_pid = lookup_new(256);
  if (!isvalid(list->py_proc_for_pid))
    goto error;

  list->ppid_for_pid = lookup_new(1024);
  if (!isvalid(list->ppid_for_pid)) {
    goto error;
  }

  // Add the parent process to the list.
  _py_proc_list__add(list, parent_py_proc);

  return list;

error:
  py_proc_list__destroy(list);
  
  return NULL;
} /* py_proc_list_new */


// ----------------------------------------------------------------------------
void
py_proc_list__add_proc_children(py_proc_list_t * self, uintptr_t ppid) {
  lookup__iteritems_start(self->ppid_for_pid, key_dt, pid, value_t, pid_ppid) {
    if (pid_ppid == (value_t) ppid && !_py_proc_list__has_pid(self, pid)) {
      py_proc_t * child_proc = py_proc_new(TRUE);
      if (child_proc == NULL)
        continue;

      if (py_proc__attach(child_proc, pid)) {
        py_proc__destroy(child_proc);
        continue;
      }

      _py_proc_list__add(self, child_proc);
      py_proc__log_version(child_proc, FALSE);
      py_proc_list__add_proc_children(self, pid);
    }
  } lookup__iter_stop(self->ppid_for_pid);
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

  for (py_proc_item_t * item = self->first; item != NULL; /* item = item->next */) {
    log_t("Sampling process with PID %d", item->py_proc->pid);
    stopwatch_start();
    if (!isvalid(item->py_proc->py_v) || fail(py_proc__sample(item->py_proc))) {
      py_proc__wait(item->py_proc);
      py_proc_item_t * next = item->next;
      _py_proc_list__remove(self, item);
      item = next;
    }
    else
      item = item->next;
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

  lookup__clear(self->ppid_for_pid);

  // Update PID table
  #if defined PL_LINUX                                               /* LINUX */
  char            buffer[1024];
  struct dirent * ent;

  cu_DIR * proc_dir = opendir("/proc");
  if (!isvalid(proc_dir)) {
    log_e("Failed to open /proc directory");
    return;
  }

  for (;;) {
    // This code is inspired by the ps util
    ent = readdir(proc_dir);
    if (!ent) break;
    if ((*ent->d_name <= '0') || (*ent->d_name > '9')) continue;

    unsigned long pid = strtoul(ent->d_name, NULL, 10);
    cu_FILE * stat_file = _procfs(pid, "stat");
    if (stat_file == NULL)
      continue;

    cu_char * line = NULL;
    size_t n = 0;
    if (getline(&line, &n, stat_file) < 0) {
      log_w("Failed to read stat file for process %d", pid);
      return;
    }
    
    char * stat = strchr(line, ')');
    if (!isvalid(stat)) {
      log_e("Failed to parse stat file for process %d", pid);
      return;
    }
    
    stat += 2;
    if (stat[0] == ' ') stat++;
    
    #ifdef __arm__
    #define STAT_FMT "%c %d"
    #else
    #define STAT_FMT "%c %ld"
    #endif

    uintptr_t ppid;
    if (sscanf(stat, STAT_FMT, (char *)buffer, &ppid) != 2) {
      log_e("Failed to parse stat file for process %d", pid);
      return;
    }

    lookup__set(self->ppid_for_pid, pid, (value_t) ppid);

  }

  #elif defined PL_MACOS                                             /* MACOS */
  cu_int * pid_list = NULL;

  int n_pids = proc_listallpids(NULL, 0);
  if (n_pids <= 0) {
    log_e("Failed to get the number of PIDs");
    return;
  }
  
  pid_list = (int *) calloc(n_pids, sizeof(int));
  if (!isvalid(pid_list)) {
    log_e("Failed to allocate memory for PID list");
    return;
  }

  if (proc_listallpids(pid_list, n_pids) == -1) {
    log_e("Failed to get list of all PIDs");
    return;
  }

  for (register int i = 0; i < n_pids; i++) {
    struct proc_bsdinfo proc;

    if (proc_pidinfo(pid_list[i], PROC_PIDTBSDINFO, 0, &proc, PROC_PIDTBSDINFO_SIZE) == -1)
      continue;

    lookup__set(self->ppid_for_pid, pid_list[i], (value_t) (uintptr_t) proc.pbi_ppid);
  }

  #elif defined PL_WIN                                                 /* WIN */
  cu_HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (h == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32 pe = { 0 };
  pe.dwSize = sizeof(PROCESSENTRY32);

  if (Process32First(h, &pe)) {
    do {
      lookup__set(self->ppid_for_pid, pe.th32ProcessID, (value_t) (uintptr_t) pe.th32ParentProcessID);
    } while (Process32Next(h, &pe));
  }
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

  lookup__destroy(self->py_proc_for_pid);
  self->py_proc_for_pid = NULL;

  lookup__destroy(self->ppid_for_pid);
  self->ppid_for_pid = NULL;
  
  free(self);
} /* py_proc_list__destroy */
