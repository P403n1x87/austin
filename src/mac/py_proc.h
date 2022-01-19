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

#include <stddef.h>
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
#include <sys/proc.h>
#include <unistd.h>

#include "../hints.h"

#define CHECK_HEAP
#define DEREF_SYM


#define SYMBOLS                        2

#define PROC_REF                        (self->extra->task_id)


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
// NOTE: This is inspired by glibc/posix/execvpe.c
extern char **environ;

static void
_maybe_script_execute(const char * file, char * const argv[]) {
  ptrdiff_t argc = 0;
  for (ptrdiff_t argc = 0; argv[argc] != NULL; argc++);

  char *new_argv[argc > 1 ? 2 + argc : 3];
  new_argv[0] = (char *) "/bin/sh";
  new_argv[1] = (char *) file;
  if (argc > 1)
    memcpy (new_argv + 2, argv + 1, argc * sizeof (char *));
  else
    new_argv[2] = NULL;
  /* Execute the shell.  */
  execve(new_argv[0], new_argv, environ);
}


#define execvpe(file, argv, environ) { \
  execvp(file, argv);                  \
  _maybe_script_execute(file, argv);   \
}


// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho64(py_proc_t * self, void * base, void * map) {
  bin_attr_t bin_attrs = 0;

  struct mach_header_64 * hdr = (struct mach_header_64 *) map;

  switch(hdr->filetype) {
  case MH_EXECUTE:
    bin_attrs |= BT_EXEC;
    break;
  case MH_DYLIB:
    bin_attrs |= BT_LIB;
    break;
  default:
    return 0;
  }

  int ncmds = hdr->ncmds;
  int s     = (hdr->magic == MH_CIGAM_64);

  int                         cmd_cnt  = 0;
  struct segment_command_64 * cmd      = map + sizeof(struct mach_header_64);

  for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
    switch (cmd->cmd) {
    case LC_SEGMENT_64:
      if (strcmp(cmd->segname, "__TEXT") == 0) {
        // NOTE: This is based on the assumption that we find this segment
        // early enough to allow later computations to use the correct value.
        base -= cmd->vmaddr;
      }
      else if (strcmp(cmd->segname, "__DATA") == 0) {
        int nsects = cmd->nsects;
        struct section_64 * sec = (struct section_64 *) ((void *) cmd + sizeof(struct segment_command_64));
        self->map.bss.size = 0;
        for (register int j = 0; j < nsects; j++) {
          if (strcmp(sec[j].sectname, "__bss") == 0) {
            self->map.bss.base = base + sec[j].addr;
            self->map.bss.size = sec[j].size;
            bin_attrs |= B_BSS;
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
        self->sym_loaded += _py_proc__check_sym(self, sym_name, (void *) (base + sym_tab[i].n_value));
      }
      cmd_cnt++;
    } // switch

    next_lc_64(cmd);
  }

  if (self->sym_loaded > 0)
    bin_attrs |= B_SYMBOLS;

  return bin_attrs;
} // _py_proc__analyze_macho64


// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho32(py_proc_t * self, void * base, void * map) {
  bin_attr_t bin_attrs = 0;

  struct mach_header * hdr = (struct mach_header *) map;

  switch(hdr->filetype) {
  case MH_EXECUTE:
    bin_attrs |= BT_EXEC;
    break;
  case MH_DYLIB:
    bin_attrs |= BT_LIB;
    break;
  default:
    return 0;
  }

  int ncmds = hdr->ncmds;
  int s     = (hdr->magic == MH_CIGAM);

  int                      cmd_cnt  = 0;
  struct segment_command * cmd      = map + sizeof(struct mach_header);

  for (register int i = 0; cmd_cnt < 2 && i < ncmds; i++) {
    switch (cmd->cmd) {
    case LC_SEGMENT:
      if (strcmp(cmd->segname, "__TEXT") == 0) {
        // NOTE: This is based on the assumption that we find this segment
        // early enough to allow later computations to use the correct value.
        base -= cmd->vmaddr;
      }
      else if (strcmp(cmd->segname, "__DATA") == 0) {
        int nsects = cmd->nsects;
        struct section * sec = (struct section *) ((void *) cmd + sizeof(struct segment_command));
        self->map.bss.size = 0;
        for (register int j = 0; j < nsects; j++) {
          if (strcmp(sec[j].sectname, "__bss") == 0) {
            self->map.bss.base = base + sec[j].addr;
            self->map.bss.size = sec[j].size;
            bin_attrs |= B_BSS;
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
        self->sym_loaded += _py_proc__check_sym(self, sym_name, (void *) (base + sym_tab[i].n_value));
      }
      cmd_cnt++;
    } // switch

    next_lc(cmd);
  }

  if (self->sym_loaded > 0)
    bin_attrs |= B_SYMBOLS;

  return bin_attrs;
} // _py_proc__analyze_macho32


// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_fat(py_proc_t * self, void * base, void * map) {
  log_t("Analyze fat binary");
  bin_attr_t bin_attrs = 0;

  void * vm_map = malloc(sizeof(struct mach_header_64));
  if (!isvalid(vm_map))
    FAIL;

  if (success(copy_memory(self->pid, base, sizeof(struct mach_header_64), vm_map))) {
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
          bin_attrs = _py_proc__analyze_macho32(self, base, (void *) hdr);
          break;

        case MH_MAGIC_64:
        case MH_CIGAM_64:
          bin_attrs = _py_proc__analyze_macho64(self, base, (void *) hdr);
          break;
        }
        break;
      }
    }
  }
  else log_e("Unable to copy memory from universal binary\n");

  free(vm_map);

  return bin_attrs;
}


// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho(py_proc_t * self, char * path, void * base, mach_vm_size_t size) {
  bin_attr_t bin_attrs = 0;

  int fd = open(path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary %s", path);
    return 0;
  }

  with_resources;

  // This would cause problem if allocated in the stack frame
  struct stat * fs  = (struct stat *) malloc(sizeof(struct stat));
  void        * map = MAP_FAILED;
  if (fstat(fd, fs) == -1) {  // Get file size
    log_e("Cannot get size of binary %s", path);
    NOK;
  }

  map = mmap(NULL, fs->st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    log_e("Cannot map binary %s", path);
    NOK;
  }

  log_t("Local Mach-O file mapping %p-%p\n", map, map+size);

  struct mach_header_64 * hdr = (struct mach_header_64 *) map;
  switch (hdr->magic) {
  case MH_MAGIC:
  case MH_CIGAM:
    bin_attrs = _py_proc__analyze_macho32(self, base, map);
    break;

  case MH_MAGIC_64:
  case MH_CIGAM_64:
    bin_attrs = _py_proc__analyze_macho64(self, base, map);
    break;

  case FAT_MAGIC:
  case FAT_CIGAM:
    bin_attrs = _py_proc__analyze_fat(self, base, map);
    break;

  default:
    self->sym_loaded = 0;
  }

release:
  if (map != MAP_FAILED) munmap(map, fs->st_size);
  sfree(fs);
  close(fd);

  retval = bin_attrs;

  released;
} // _py_proc__analyze_macho


// ----------------------------------------------------------------------------
static int
check_pid(pid_t pid) {
  // The best way of checking would probably be with a call to proc_find, but
  // this seems to require the Kernel framework.
  struct proc_bsdinfo proc;
  proc.pbi_status = SIDL;

  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &proc, PROC_PIDTBSDINFO_SIZE) == -1) {
    set_error(EPROCNPID);
    FAIL;
  }

  log_t("check_pid :: %d", proc.pbi_status);

  if (proc.pbi_status == SIDL || proc.pbi_status == 32767) {
    set_error(EPROCNPID);
    FAIL;
  }

  SUCCESS;
}


// ----------------------------------------------------------------------------
static mach_port_t
pid_to_task(pid_t pid) {
  mach_port_t   task;
  kern_return_t result;

  if (fail(check_pid(pid))) {
    log_d("No such process: %d", pid);
    FAIL;
  }

  result = task_for_pid(mach_task_self(), pid, &task);
  if (result != KERN_SUCCESS) {
    log_d("Call to task_for_pid failed: %s", mach_error_string(result));
    set_error(EPROCPERM);
    FAIL;
  }
  return task;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_maps(py_proc_t * self) {
  mach_vm_address_t              address = 0;
  mach_vm_size_t                 size = 0;
  vm_region_basic_info_data_64_t region_info;
  mach_msg_type_number_t         count = sizeof(vm_region_basic_info_data_64_t);
  mach_port_t                    object_name;

  char * path = (char *) calloc(MAXPATHLEN + 1, sizeof(char));
  if (!isvalid(path))
    FAIL;

  with_resources;

  // NOTE: Mac OS X kernel bug. This also gives time to the VM maps to
  // stabilise.
  usleep(50000);

  self->extra->task_id = pid_to_task(self->pid);
  if (self->extra->task_id == 0)
    NOK;

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  sfree(self->bin_path);
  self->bin_path = NULL;

  sfree(self->lib_path);
  self->lib_path = NULL;

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

    int len      = proc_regionfilename(self->pid, address, path, MAXPATHLEN);
    int path_len = strlen(path);

    if (size > 0 && len && !self->sym_loaded) {
      path[len] = 0;

      bin_attr_t bin_attrs = _py_proc__analyze_macho(self, path, (void *) address, size);
      if (bin_attrs & B_SYMBOLS) {
        if (size < BINARY_MIN_SIZE) {
          // We found the symbols in the binary but we are probably going to use the wrong base
          // since the map is too small. So pretend we didin't find them.
          self->sym_loaded = 0;
        }
        else {
          switch (BINARY_TYPE(bin_attrs)) {
          case BT_EXEC:
            if (self->bin_path == NULL) {
              self->bin_path = strndup(path, path_len);
              log_d("Candidate binary: %s", self->bin_path);
            }
            break;
          case BT_LIB:
            if (self->lib_path == NULL && size > BINARY_MIN_SIZE) {
              self->lib_path = strndup(path, path_len);
              log_d("Candidate library: %s", self->lib_path);
            }
          }
        }
      }
    }

    // Make a best guess for the heap boundary.
    if (self->map.heap.base == NULL)
      self->map.heap.base = (void *) address;
    self->map.heap.size += size;

    address += size;
  }

  log_d("BSS bounds  [%p - %p]", self->map.bss.base, self->map.bss.base + self->map.bss.size);
  log_d("HEAP bounds [%p - %p]", self->map.heap.base, self->map.heap.base + self->map.heap.size);

  retval = !self->sym_loaded;

release:
  free(path);

  released;
} // _py_proc__get_maps


// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t * self) {
  struct mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

  return task_info(
    self->extra->task_id, MACH_TASK_BASIC_INFO, (task_info_t) &info, &count
  ) == KERN_SUCCESS
    ? info.resident_size
    : -1;
} // _py_proc__get_resident_memory


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  log_t("macOS: py_proc init");
  if (!isvalid(self))
    FAIL;

  return _py_proc__get_maps(self);
} // _py_proc__init

#endif
