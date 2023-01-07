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
#include <mach/machine.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "../hints.h"
#include "../resources.h"

#define CHECK_HEAP
#define DEREF_SYM


#define SYMBOLS                        2


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
  // void
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
  cpu_type_t cpu;
  int is_abi64;
  size_t cpu_size = sizeof(cpu), abi64_size = sizeof(is_abi64);

  sysctlbyname("hw.cputype", &cpu, &cpu_size, NULL, 0);
  sysctlbyname("hw.cpu64bit_capable", &is_abi64, &abi64_size, NULL, 0);

  cpu |= is_abi64 * CPU_ARCH_ABI64;

  // Look up corresponding part from universal binary
  struct fat_header * fat_hdr = (struct fat_header *) map;

  int fs = fat_hdr->magic == FAT_CIGAM;
  struct fat_arch * arch = (struct fat_arch *) (map + sizeof(struct fat_header));

  uint32_t narchs = sw32(fs, fat_hdr->nfat_arch);
  log_d("Found %d architectures in fat binary", narchs);

  for (register int i = 0; i < narchs; i++) {
    log_d("- architecture %x (expected %x)", sw32(fs, arch[i].cputype), cpu);
    
    if (sw32(fs, arch[i].cputype) == cpu) {
      struct mach_header_64 * hdr = (struct mach_header_64 *) (map + sw32(fs, arch[i].offset));
      
      switch (hdr->magic) {
      case MH_MAGIC:
      case MH_CIGAM:
        log_d("Using the 32-bit image of the fat binary");
        return _py_proc__analyze_macho32(self, base, (void *) hdr);

      case MH_MAGIC_64:
      case MH_CIGAM_64:
        log_d("Using the 64-bit image of the fat binary");
        return _py_proc__analyze_macho64(self, base, (void *) hdr);
      
      default:
        log_e("Unexpected Mach-O magic");
        return INVALID_ATTR;
      }
      
      break;
    }
  }

  log_e("Fat binary has no matching architectures");

  return INVALID_ATTR;
}


// ----------------------------------------------------------------------------
static bin_attr_t
_py_proc__analyze_macho(py_proc_t * self, char * path, void * base, mach_vm_size_t size) {
  cu_fd fd = open(path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary %s", path);
    return INVALID_ATTR;
  }
  
  log_d("Analysing binary %s", path);
  
  // This would cause problem if allocated in the stack frame
  cu_void     * fs_buffer = malloc(sizeof(struct stat));
  struct stat * fs        = (struct stat *) fs_buffer;
  cu_map_t    * map       = NULL;
  if (fstat(fd, fs) == -1) {  // Get file size
    log_e("Cannot get size of binary %s", path);
    FAIL;  // cppcheck-suppress [memleak]
  }

  map = map_new(fd, fs->st_size, MAP_SHARED);
  if (!isvalid(map)) {
    log_e("Cannot map binary %s", path);
    FAIL;  // cppcheck-suppress [memleak]
  }

  void * map_addr = map->addr;
  log_t("Local Mach-O file mapping %p-%p\n", map_addr, map_addr + size);

  struct mach_header_64 * hdr = (struct mach_header_64 *) map_addr;
  switch (hdr->magic) {
  case MH_MAGIC:
  case MH_CIGAM:
    log_d("Binary is Mach-O 32");
    return _py_proc__analyze_macho32(self, base, map_addr);

  case MH_MAGIC_64:
  case MH_CIGAM_64:
    log_d("Binary is Mach-O 64");
    return _py_proc__analyze_macho64(self, base, map_addr);

  case FAT_MAGIC:
  case FAT_CIGAM:
    log_d("Binary is fat");
    return _py_proc__analyze_fat(self, base, map_addr);

  default:
    self->sym_loaded = 0;
  }

  return 0;
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
  mach_vm_size_t                 size    = 0;
  mach_msg_type_number_t         count   = sizeof(vm_region_basic_info_data_64_t);
  vm_region_basic_info_data_64_t region_info;
  mach_port_t                    object_name;

  cu_char * path = (char *) calloc(MAXPATHLEN + 1, sizeof(char));
  if (!isvalid(path))
    FAIL;

  // NOTE: Mac OS X kernel bug. This also gives time to the VM maps to
  // stabilise.
  usleep(50000);

  self->proc_ref = pid_to_task(self->pid);
  if (self->proc_ref == 0)
    FAIL;  // cppcheck-suppress [memleak]

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  sfree(self->bin_path);
  sfree(self->lib_path);

  while (mach_vm_region(
    self->proc_ref,
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

  return !self->sym_loaded;
} // _py_proc__get_maps


// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t * self) {
  struct mach_task_basic_info info;
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

  return task_info(
    self->proc_ref, MACH_TASK_BASIC_INFO, (task_info_t) &info, &count
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
