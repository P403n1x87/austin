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
#include <mach-o/nlist.h>
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


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho64(py_proc_t * self, void * map) {
  struct mach_header_64 * hdr = (struct mach_header_64 *) map;
  int ncmds = hdr->ncmds;
  int s = hdr->magic == MH_CIGAM_64;

  int cmd_cnt = 0;
  struct segment_command_64 * cmd = map + sizeof(struct mach_header_64);
  int hit_cnt = 0;
  void * img_base = self->map.bss.base;
  for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
    switch (cmd->cmd) {
    case LC_SEGMENT_64:
      if (strcmp(cmd->segname, "__DATA") == 0) {
        int nsects = cmd->nsects;
        struct section_64 * sec = (struct section_64 *) ((void *) cmd + sizeof(struct segment_command_64));
        self->map.bss.size = 0;
        for (register int j = 0; j < nsects; j++) {
          if (strcmp(sec[j].sectname, "__bss") == 0) {
            self->map.bss.base += sec[j].addr;
            self->map.bss.size = sec[j].size;
            break;
          }
        }
        cmd_cnt++;
      }
      break;

    case LC_SYMTAB:
      for (
        register int i = 0;
        hit_cnt == 0 && i < sw32(s, ((struct symtab_command *) cmd)->nsyms);
        i++
      ) {
        struct nlist_64 * sym_tab = (struct nlist_64 *) (map + sw32(s, ((struct symtab_command *) cmd)->symoff));
        void     * str_tab = (void *) (map + sw32(s, ((struct symtab_command *) cmd)->stroff));

        // TODO: Assess quality
        if ((sym_tab[i].n_type & N_EXT) == 0)
          continue;

        char * sym_name = (char *) (str_tab + sym_tab[i].n_un.n_strx);
        long   hash     = string_hash(sym_name);
        for (register int j = 0; j < DYNSYM_COUNT; j++) {
          if (hash == _dynsym_hash_array[j] && strcmp(sym_name, _dynsym_array[j]) == 0) {
            *(&(self->tstate_curr_raddr) + j) = (void *) (img_base + sym_tab[i].n_value);
            hit_cnt++;
            self->sym_loaded = 1;
            #ifdef DEBUG
            log_d("Symbol %s found at %p", sym_name, (void *) (img_base + sym_tab[i].n_value));
            #endif
          }
        }
      }
      cmd_cnt++;
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
    for (register int i = 0; i < narchs; i++) {
      if (sw32(fs, arch[i].cputype) == sw32(ms, cpu)) {
        hdr = (struct mach_header_64 *) (map + sw32(fs, arch[i].offset));
        switch (hdr->magic) {
        case MH_MAGIC:
        case MH_CIGAM:
          break;
          // _py_proc__analyze_macho32(self, (void *) hdr);

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
  log_d("Local mapping %p-%p\n", map, map+size);
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

  // TODO: Fix loop
  while (1) {
    kern_return_t retval = mach_vm_region(
      pid_to_task(self->pid), // TODO: task,
      &address,
      &size,
      VM_REGION_BASIC_INFO,
      (vm_region_info_t) &region_info,
      &count,
      &object_name
    );

    if (retval != KERN_SUCCESS)
      break;

    char path[MAXPATHLEN];
    if (size > 0 && proc_regionfilename(self->pid, address, path, MAXPATHLEN)) {
      printf("%p-%p (%lluK) %s\n", (void *) address, (void *) address+size, size >> 10, path);
      if (self->bin_path == NULL && strstr(path, "python")) {
        self->bin_path = (char *) malloc(strlen(path) + 1);
        strcpy(self->bin_path, path);
        #ifdef DEBUG
        log_d("Python binary: %s", self->bin_path);
        #endif
      } else if (size > (1 << 12) && strstr(path, "Python"))
        return _py_proc__analyze_macho(self, path, (void *) address, size);
    }
    address += size;
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

    if (self->maps_loaded == 0)
      return 1;
  }

  return 0;
}

#endif
