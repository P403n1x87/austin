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

#include <winternl.h>
#include <Ntstatus.h>

#include "../py_thread.h"


static PVOID _pi_buffer      = NULL;
static ULONG _pi_buffer_size = 0;


// ----------------------------------------------------------------------------
static int
_py_thread__is_idle(py_thread_t * self) {
  ULONG n;
  NTSTATUS status = NtQuerySystemInformation(
    SystemProcessInformation,
    _pi_buffer,
    _pi_buffer_size,
    &n
  );
  if (status == STATUS_INFO_LENGTH_MISMATCH) {
    // Buffer was too small so we reallocate a larger one and try again.
    _pi_buffer_size = n;
    PVOID _new_buffer = realloc(_pi_buffer, n);
    if (!isvalid(_new_buffer)) {
      log_d("cannot reallocate process info buffer");
      return -1;
    }
    _pi_buffer = _new_buffer;
    return _py_thread__is_idle(self);
  }
  if (status != STATUS_SUCCESS) {
    log_d("[NtQuerySystemInformation] Cannot get the status of running threads: %d.", status);
    return -1;
  }

  SYSTEM_PROCESS_INFORMATION * pi = (SYSTEM_PROCESS_INFORMATION *) _pi_buffer;
  while (pi->UniqueProcessId != (HANDLE) self->proc->pid) {
    if (pi->NextEntryOffset == 0) {
      // We didn't find the process, which shouldn't really happen
      log_d("[NtQuerySystemInformation] Process %ld not found", self->proc->pid);
      return -1;
    }
    pi = (SYSTEM_PROCESS_INFORMATION *) (((BYTE *) pi) + pi->NextEntryOffset);
  }
  log_t("[NtQuerySystemInformation] Process info found for PID %d", self->proc->pid);

  SYSTEM_THREADS * ti = (SYSTEM_THREADS *) ((char *) pi + sizeof(SYSTEM_PROCESS_INFORMATION));
  for (register int i = 0; i < pi->NumberOfThreads; i++, ti++) {
    if (ti->ClientId.UniqueThread == (HANDLE) self->tid) {
      log_t("[NtQuerySystemInformation] Thread info found for TID %d", self->tid);
      return ti->State != StateRunning;
    }
  }

  return -1;
}
