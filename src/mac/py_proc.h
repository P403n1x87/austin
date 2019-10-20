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

#include "../error.h"


#define CHECK_HEAP
#define DEREF_SYM


#define SYMBOLS              2


#define next_lc(cmd)    (cmd = (struct segment_command *)    ((void *) cmd + cmd->cmdsize));
#define next_lc_64(cmd) (cmd = (struct segment_command_64 *) ((void *) cmd + cmd->cmdsize));

// ---- Endianness ----
#define ENDIAN(f, v) (f ? __bswap_32(v) : v)

#define bswap_16(value) \
((((value) & 0xff) << 8) | ((value) >> 8))

#define bswap_32(value) \
(((uint32_t)bswap_16((uint16_t)((value) & 0xffff)) << 16) | \
(uint32_t)bswap_16((uint16_t)((value) >> 16)))

#define sw32(f, v) (f ? bswap_32(v) : v)


struct _proc_extra_info {
  mach_port_t task_id;
};


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho64(py_proc_t * self, void * map) {
  struct mach_header_64 * hdr = (struct mach_header_64 *) map;

  int ncmds = hdr->ncmds;
  int s     = (hdr->magic == MH_CIGAM_64);

  int cmd_cnt = 0;
  struct segment_command_64 * cmd = map + sizeof(struct mach_header_64);
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
        self->sym_loaded < SYMBOLS && i < sw32(s, ((struct symtab_command *) cmd)->nsyms);
        i++
      ) {
        struct nlist_64 * sym_tab = (struct nlist_64 *) (map + sw32(s, ((struct symtab_command *) cmd)->symoff));
        void            * str_tab = (void *)            (map + sw32(s, ((struct symtab_command *) cmd)->stroff));

        // TODO: Assess quality
        if ((sym_tab[i].n_type & N_EXT) == 0)
          continue;

        char * sym_name = (char *) (str_tab + sym_tab[i].n_un.n_strx);
        self->sym_loaded += _py_proc__check_sym(self, sym_name, (void *) (img_base + sym_tab[i].n_value));
      }
      cmd_cnt++;
    } // switch

    next_lc_64(cmd);
  }

  if (self->map.bss.size == 0)
    return 1;

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho32(py_proc_t * self, void * map) {
  struct mach_header * hdr = (struct mach_header *) map;

  int ncmds = hdr->ncmds;
  int s     = (hdr->magic == MH_CIGAM);

  int cmd_cnt = 0;
  struct segment_command * cmd = map + sizeof(struct mach_header);
  void * img_base = self->map.bss.base;
  for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
    switch (cmd->cmd) {
    case LC_SEGMENT:
      if (strcmp(cmd->segname, "__DATA") == 0) {
        int nsects = cmd->nsects;
        struct section * sec = (struct section *) ((void *) cmd + sizeof(struct segment_command));
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
        self->sym_loaded < SYMBOLS && i < sw32(s, ((struct symtab_command *) cmd)->nsyms);
        i++
      ) {
        struct nlist * sym_tab = (struct nlist *) (map + sw32(s, ((struct symtab_command *) cmd)->symoff));
        void         * str_tab = (void *)         (map + sw32(s, ((struct symtab_command *) cmd)->stroff));

        // TODO: Assess quality
        if ((sym_tab[i].n_type & N_EXT) == 0)
          continue;

        char * sym_name = (char *) (str_tab + sym_tab[i].n_un.n_strx);
        self->sym_loaded += _py_proc__check_sym(self, sym_name, (void *) (img_base + sym_tab[i].n_value));
      }
      cmd_cnt++;
    } // switch

    next_lc(cmd);
  }

  if (self->map.bss.size == 0)
    return 1;

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_fat(py_proc_t * self, void * addr, void * map) {
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
          _py_proc__analyze_macho32(self, (void *) hdr);

        case MH_MAGIC_64:
        case MH_CIGAM_64:
          _py_proc__analyze_macho64(self, (void *) hdr);
          break;
        }
        break;
      }
    }
  }
  else log_e("Unable to copy memory from universal binary\n");

  free(vm_map);

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_macho(py_proc_t * self, char * path, void * addr, mach_vm_size_t size) {
  int fd = open(path, O_RDONLY);

  // This would cause problem if allocated in the stack frame
  struct stat * fs = (struct stat *) malloc(sizeof(struct stat));
  fstat(fd, fs);  // Get file size

  void * map = mmap(NULL, fs->st_size, PROT_READ, MAP_SHARED, fd, 0);
  free(fs);

  log_t("Local Mach-O file mapping %p-%p\n", map, map+size);

  struct mach_header_64 * hdr = (struct mach_header_64 *) map;
  switch (hdr->magic) {
  case MH_MAGIC:
  case MH_CIGAM:
    break;
    _py_proc__analyze_macho32(self, map);

  case MH_MAGIC_64:
  case MH_CIGAM_64:
    _py_proc__analyze_macho64(self, map);
    break;

  case FAT_MAGIC:
  case FAT_CIGAM:
    _py_proc__analyze_fat(self, addr, map);
    break;

  default:
    self->sym_loaded = 0;
  }

  munmap(map, size);
  close(fd);

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static mach_port_t
pid_to_task(pid_t pid) {
  mach_port_t task;
  if (task_for_pid(mach_task_self(), pid, &task) != KERN_SUCCESS) {
    log_e("Insufficient permissions to call task_for_pid.");
    error = EPROCPERM;
    return 0;
  }
  return task;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_maps(py_proc_t * self) {
  mach_vm_address_t              address = 0;
  mach_vm_size_t                 size;
  vm_region_basic_info_data_64_t region_info;
  mach_msg_type_number_t         count = sizeof(vm_region_basic_info_data_64_t);
  mach_port_t                    object_name;

  usleep(10000);  // NOTE: Mac OS X kernel bug

  self->extra->task_id = pid_to_task(self->pid);
  if (self->extra->task_id == 0)
    return 1;

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  while (mach_vm_region(
    self->extra->task_id,
    &address,
    &size,
    VM_REGION_BASIC_INFO_64,
    (vm_region_info_t) &region_info,
    &count,
    &object_name
  ) == KERN_SUCCESS) {
    if ((void *) address < self->min_raddr)
      self->min_raddr = (void *) address;

    if ((void *) address + size > self->max_raddr)
      self->max_raddr = (void *) address + size;

    char path[MAXPATHLEN];
    int len = proc_regionfilename(self->pid, address, path, MAXPATHLEN);
    int path_len = strlen(path);

    if (size > 0 && len) {
      path[len] = 0;
      if (self->bin_path == NULL && strstr(path, "python")) {
        if (strstr(path + path_len - 3, ".so") == NULL) {
          // check that it is not a .so file
          self->bin_path = strndup(path, path_len);
        }
      }

      if (self->lib_path == NULL && strstr(path, "Python")) {
        if (strstr(path + path_len - 3, ".so") == NULL) {
          self->lib_path = strndup(path, path_len);

          self->map.bss.base = (void *) address;  // WARNING: Partial result. Not yet the BSS base!!
          if (_py_proc__analyze_macho(self, path, (void *) address, size))
            return 1;
        }
      }
    }

    address += size;
  }

  if (self->bin_path && self->lib_path && !strcmp(self->bin_path, self->lib_path))
    self->bin_path = NULL;

  return !self->sym_loaded;
}


// ----------------------------------------------------------------------------
static ssize_t _py_proc__get_resident_memory(py_proc_t * self) {
  struct mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

  return task_info(
    self->extra->task_id, MACH_TASK_BASIC_INFO, (task_info_t) &info, &count
  ) == KERN_SUCCESS
    ? info.resident_size
    : -1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (self == NULL)
    return 1;

  return _py_proc__get_maps(self);
}

#endif
