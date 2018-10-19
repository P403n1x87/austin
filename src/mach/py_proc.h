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

#ifdef PY_PROC_C

// https://www.real-world-systems.com/docs/vmmap.example.html
// https://github.com/rbspy/proc-maps/blob/master/src/mac_maps/mod.rs
// https://stackoverflow.com/questions/8058005/mac-os-x-equivalent-of-virtualquery-or-proc-pid-maps


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho64(py_proc_t * self, void * map) {
  int ncmds = (mach_header_64 *) map->ncmds;

  segment_command_64 * cmd = map + sizeof(mach_header_64);
  for (register int i = 0; i < ncmds; i++) {
    if (cmd->cmd != LC_SEGMENT)
      continue;

    if (strcmp(cmd->segname, "__DATA") == 0) {
      int nsects = cmd->nsects;
      section_64 * sec = (section_64 *) ((void *) cmd + sizeof(segment_command_64));
      self->map.bss.base = NULL;
      for (register int j = 0; self->map.bss.base == NULL && j < nsects; j++) {
        if (strcmp(sec->sectname, "__bss") == 0) {
          self->map.bss.base = sec->addr;
          self->map.bss.size = sec->size;
          break;
        }
      }
    }
    cmd = (segment_command *) ((void *) cmd + cmd->cmdsize);
  }

  if (self->map.bss.base == NULL)
    return 1;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho(py_proc_t * self, char * path) {
  // map file to memory
  //
  // get location of bss section
  //
  // unmap file

  int    fd  = open(path, O_RDONLY);
  void * map = mmap(NULL, elf_map_size, PROT_READ, MAP_SHARED, fd, 0);

  mach_header hdr = (mach_header *) map;
  switch (hdr.magic) {
  case MH_MAGIC:
    _py_proc__analyze_macho32(self, map);

  case MH_MAGIC_64:
    _py_proc__analyze_macho64(Self, map);

  default:
    // TODO: Handle invalid magic
  }

  munmap(map);
  close(fd);
}


// ----------------------------------------------------------------------------
task_t
pid_to_task(pid_t pid) {
  task_t task;
  return task_for_pid(mach_task_self(), pid, &task) == KERN_SUCCESS ? task : NULL;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_maps(py_proc_t * self) {
  // Call mach_vm_region repeatedly to get all the maps
  // For each map, call proc_regionfilename to get the binary path

  mach_vm_address_t           address = 1;
  mach_vm_size_t              size;
  vm_region_basic_info_data_t region_info;
  mach_msg_type_number_t      count;
  mach_port_t                 object_name;

  while (mach_vm_region(
    pid_to_task(self->pid),
    &address,
    &size,
    VM_REGION_BASIC_INFO,
    &region_info,
    &count,
    &object_name
  ) == KERN_SUCCESS) {
    char path[MAXPATHLEN];
    if (proc_regionfilename(self->pid, address, path, MAXPATHLEN)) {
      // Analyse file name
      // TODO: Call _py_proc__analyze_macho
    }
    address += size;
  }
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_bin(py_proc_t * self) {
  if (self == NULL)
    return 1;

  if (self->maps_loaded == 0) {
    self->maps_loaded = 1 - _py_proc__get_maps(self);

    if (self->maps_loaded == 0)
      return 1;
  }

  // TODO: Not loading symbols on MacOS for now
  self->sym_loaded = 1;

  return 0;
}

#endif
