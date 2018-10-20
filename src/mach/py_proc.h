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

#include <stdio.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <unistd.h>


#define ENDIAN(f, v) (f ? __bswap_32(v) : v)

#define bswap_16(value) \
((((value) & 0xff) << 8) | ((value) >> 8))

#define bswap_32(value) \
(((uint32_t)bswap_16((uint16_t)((value) & 0xffff)) << 16) | \
(uint32_t)bswap_16((uint16_t)((value) >> 16)))

#define sw32(f, v) (f ? bswap_32(v) : v)

// https://www.real-world-systems.com/docs/vmmap.example.html
// https://github.com/rbspy/proc-maps/blob/master/src/mac_maps/mod.rs
// https://stackoverflow.com/questions/8058005/mac-os-x-equivalent-of-virtualquery-or-proc-pid-maps


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho64(py_proc_t * self, void * map) {
  struct mach_header_64 * hdr = (struct mach_header_64 *) map;
  int ncmds = hdr->ncmds;
  int s = hdr->magic == MH_CIGAM_64;
  if (hdr->magic == MH_MAGIC_64 || hdr->magic == MH_CIGAM_64)
    printf("Found MACHO64 with %d commands (%d)\n", sw32(s,ncmds), s);

  struct segment_command_64 * cmd = map + sizeof(struct mach_header_64);
  for (register int i = 0; i < ncmds; i++) {
    printf("Found command: %x of size %d @ %p (%s)\n", cmd->cmd, sw32(s,cmd->cmdsize), cmd, cmd->segname);
    if (cmd->cmd == LC_SEGMENT_64) {
      if (strcmp(cmd->segname, "__DATA") == 0) {
        printf("Found __DATA\n");
        int nsects = cmd->nsects;
        struct section_64 * sec = (struct section_64 *) ((void *) cmd + sizeof(struct segment_command_64));
        self->map.bss.size = 0;
        for (register int j = 0; j < nsects; j++) {
          printf("Section %s\n", sec[j].sectname);
          if (strcmp(sec[j].sectname, "__bss") == 0) {
            self->map.bss.base += sec[j].addr;
            self->map.bss.size = sec[j].size;
            printf("Found bss. Address: %p, size: %zdK\n", self->map.bss.base, self->map.bss.size >> 10);
            break;
          }
        }
        break;
      }
    }
    cmd = (struct segment_command_64 *) ((void *) cmd + cmd->cmdsize);
  }

  if (self->map.bss.size == 0)
    return 1;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_fat(py_proc_t * self, void * addr, void * map) {
  int retval = 0;

  void * vm_map = malloc(sizeof(struct mach_header_64));
  if (vm_map == NULL)
    return 1;

  if (copy_memory(self->pid, addr, sizeof(struct mach_header_64), vm_map) == sizeof(struct mach_header_64)) {
    // Determine CPU type from process in memory
    struct mach_header_64 * hdr = (struct mach_header_64 *) vm_map;
    int ms = hdr->magic == MH_CIGAM || hdr->magic == MH_CIGAM_64;  // This is probably useless
    cpu_type_t cpu = hdr->cputype;

    // Look up corresponding part from universal binary
    struct fat_header * fat_hdr = (struct fat_header *) map;

    int fs = fat_hdr->magic == FAT_CIGAM;
    struct fat_arch * arch = (struct fat_arch *) (map + sizeof(struct fat_header));

    uint32_t narchs = sw32(fs, fat_hdr->nfat_arch);
printf("There are %d architectures in FAT binary\n", narchs);
    for (register int i = 0; i < narchs; i++) {
      printf("%x == %x\n", sw32(fs, arch[i].cputype), sw32(ms, cpu));
      if (sw32(fs, arch[i].cputype) == sw32(ms, cpu)) {
        hdr = (struct mach_header_64 *) (map + sw32(fs, arch[i].offset));
        printf("%p->%p (%dK)\n", map, hdr, sw32(fs, arch[i].offset)>> 10);
        // self->map.bss.base = (void *) addr;
        switch (hdr->magic) {
        case MH_MAGIC:
        case MH_CIGAM:
          break;
          // _py_proc__analyze_macho32(self, map);

        case MH_MAGIC_64:
        case MH_CIGAM_64:
          _py_proc__analyze_macho64(self, (void *) hdr);
          break;

        default:
          // TODO: Handle invalid magic
          printf("Invalid format from FAT\n");
          return 1;
        }
        break;
      }
    }
  } else {
    printf("Unable to copy memory\n");
    retval = 1;
  }

  free(vm_map);
  return retval;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho(py_proc_t * self, char * path, void * addr, mach_vm_size_t size) {
  int    fd  = open(path, O_RDONLY);
  struct stat * fs = (struct stat *) malloc(sizeof(struct stat));
  fstat(fd, fs);
  void * map = mmap(NULL, fs->st_size, PROT_READ, MAP_SHARED, fd, 0);
  free(fs);
  printf("Local mapping %p-%p\n", map, map+size);
  struct mach_header_64 * hdr = (struct mach_header_64 *) map;
  self->map.bss.base = addr;
  switch (hdr->magic) {
  case MH_MAGIC:
  case MH_CIGAM:
    break;
    // _py_proc__analyze_macho32(self, map);

  case MH_MAGIC_64:
  case MH_CIGAM_64:
    _py_proc__analyze_macho64(self, map);
    break;

  case FAT_MAGIC:
  case FAT_CIGAM:
    printf("Fat\n");
    _py_proc__analyze_fat(self, addr, map);
    break;

  default:
    // TODO: Handle invalid magic
    printf("Invalid format\n");
    return 1;
  }

  munmap(map, size);
  close(fd);

  return 0;
}


// ----------------------------------------------------------------------------
static mach_port_t
pid_to_task(pid_t pid) {
  mach_port_t task;
  task_for_pid(mach_task_self(), pid, &task);
  return task;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_maps(py_proc_t * self) {
  // Call mach_vm_region repeatedly to get all the maps
  // For each map, call proc_regionfilename to get the binary path

  mach_vm_address_t           address = 1;
  mach_vm_size_t              size;
  vm_region_basic_info_data_t region_info;
  mach_msg_type_number_t      count = sizeof(vm_region_basic_info_data_t);
  mach_port_t                 object_name;

  // NOTE: Mac OS X kernel bug
  usleep(10000);

  mach_port_t task = pid_to_task(self->pid);
register int i = 0;
  while (1) {
    kern_return_t retval = mach_vm_region(
      pid_to_task(self->pid), //task,
      &address,
      &size,
      VM_REGION_BASIC_INFO,
      (vm_region_info_t) &region_info,
      &count,
      &object_name
    );

    if (retval != KERN_SUCCESS/* || size == 0*/)
      break;

    char path[MAXPATHLEN];
    if (size > 0 && proc_regionfilename(self->pid, address, path, MAXPATHLEN)) {
      printf("%p-%p (%lluB) %s\n", (void *) address, (void *) address+size, size, path);
      if (self->bin_path == NULL && strstr(path, "python")) {
        self->bin_path = (char *) malloc(strlen(path) + 1);
        strcpy(self->bin_path, path);
        // #ifdef DEBUG
        log_d("Python binary: %s", self->bin_path);
        // #endif
      } else if (size > (1 << 12) && strstr(path, "Python"))
        return _py_proc__analyze_macho(self, path, (void *) address, size);
    }
    log_d("incrementing %d", ++i);
    address += size + 4097;
  }

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_bin(py_proc_t * self) {
  if (self == NULL)
    return 1;

  if (self->maps_loaded == 0) {
    self->maps_loaded = 1 - _py_proc__get_maps(self);
// exit(-42);
    if (self->maps_loaded == 0)
      return 1;
  }

  // TODO: Not loading symbols on MacOS for now
  self->sym_loaded = 1;

  return 0;
}

#endif
