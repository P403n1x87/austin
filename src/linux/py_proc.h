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

#include <elf.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef NATIVE
#include "../argparse.h"
#endif
#include "../dict.h"
#include "../hints.h"
#include "../py_proc.h"


#define CHECK_HEAP
#define DEREF_SYM


#define BIN_MAP                  (1 << 0)
#define DYNSYM_MAP               (1 << 1)
#define RODATA_MAP               (1 << 2)
#define HEAP_MAP                 (1 << 3)
#define BSS_MAP                  (1 << 4)

#define SYMBOLS                        2

#define PROC_REF                        (self->pid)


// Get the offset of the ith section header
#define ELF_SH_OFF(ehdr, i) /* as */ (ehdr->e_shoff + i * ehdr->e_shentsize)


struct _proc_extra_info {
  unsigned int page_size;
  char         statm_file[24];
  pthread_t    wait_thread_id;
};


union {
  Elf32_Ehdr v32;
  Elf64_Ehdr v64;
} ehdr_v;


// ----------------------------------------------------------------------------
static void *
wait_thread(void * py_proc) {
  waitpid(((py_proc_t *) py_proc)->pid, 0, 0);
  return NULL;
}


// ----------------------------------------------------------------------------
static ssize_t
_file_size(char * file) {
  struct stat statbuf;

  stat(file, &statbuf);

  return statbuf.st_size;
}


// ----------------------------------------------------------------------------
static Elf64_Addr
_get_base_64(Elf64_Ehdr * ehdr, void * elf_map)
{
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    Elf64_Phdr * phdr = (Elf64_Phdr *) (elf_map + ehdr->e_phoff + i * ehdr->e_phentsize);
    if (phdr->p_type == PT_LOAD)
      return phdr->p_vaddr - phdr->p_vaddr % phdr->p_align;
  }
  return UINT64_MAX;
} /* _get_base_64 */


static int
_py_proc__analyze_elf64(py_proc_t * self, void * elf_map) {
  register int symbols = 0;

  Elf64_Ehdr * ehdr = elf_map;

  // Section header must be read from binary as it is not loaded into memory
  Elf64_Xword   sht_size      = ehdr->e_shnum * ehdr->e_shentsize;
  Elf64_Off     elf_map_size  = ehdr->e_shoff + sht_size;
  Elf64_Shdr  * p_shdr;

  Elf64_Shdr  * p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr->e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;
  Elf64_Shdr  * p_dynsym     = NULL;
  Elf64_Addr    base         = _get_base_64(ehdr, elf_map);

  if (base != UINT64_MAX) {
    log_d("Base @ %p", base);

    for (Elf64_Off sh_off = ehdr->e_shoff; \
      sh_off < elf_map_size; \
      sh_off += ehdr->e_shentsize \
    ) {
      p_shdr = (Elf64_Shdr *) (elf_map + sh_off);

      if (
        p_shdr->sh_type == SHT_DYNSYM && \
        strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0
      ) {
        p_dynsym = p_shdr;
      }
      // NOTE: This might be required if the Python version must be retrieved
      //       from the RO data section
      // else if (
      //   p_shdr->sh_type == SHT_PROGBITS &&
      //   strcmp(sh_name_base + p_shdr->sh_name, ".rodata") == 0
      // ) {
      //   self->map.rodata.base = (void *) p_shdr->sh_offset;
      //   self->map.rodata.size = p_shdr->sh_size;
      //   map_flag |= RODATA_MAP;
      // }
      else if (strcmp(sh_name_base + p_shdr->sh_name, ".bss") == 0) {
        self->map.bss.base = self->map.elf.base + (p_shdr->sh_addr - base);
        self->map.bss.size = p_shdr->sh_size;
        log_d("BSS @ %p, (size %x)", self->map.bss.base, self->map.bss.size);
      }
    }

    if (p_dynsym != NULL) {
      if (p_dynsym->sh_offset != 0) {
        Elf64_Shdr * p_strtabsh = (Elf64_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

        // Search for dynamic symbols
        for (Elf64_Off tab_off = p_dynsym->sh_offset; \
          tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
          tab_off += p_dynsym->sh_entsize
        ) {
          Elf64_Sym * sym      = (Elf64_Sym *) (elf_map + tab_off);
          char      * sym_name = (char *) (elf_map + p_strtabsh->sh_offset + sym->st_name);
          void      * value    = self->map.elf.base + (sym->st_value - base);
          if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= SYMBOLS)
            break;
        }
      }
    }
  }

  if (!symbols) {
    log_e("ELF binary has no Python symbols");
    FAIL;
  }

  SUCCESS;
} /* _py_proc__analyze_elf64 */


// ----------------------------------------------------------------------------
static Elf32_Addr
_get_base_32(Elf32_Ehdr * ehdr, void * elf_map)
{
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    Elf32_Phdr * phdr = (Elf32_Phdr *) (elf_map + ehdr->e_phoff + i * ehdr->e_phentsize);
    if (phdr->p_type == PT_LOAD)
      return phdr->p_vaddr - phdr->p_vaddr % phdr->p_align;
  }
  return UINT32_MAX;
} /* _get_base_32 */


static int
_py_proc__analyze_elf32(py_proc_t * self, void * elf_map) {
  register int symbols = 0;

  Elf32_Ehdr * ehdr = elf_map;

  // Section header must be read from binary as it is not loaded into memory
  Elf32_Xword   sht_size      = ehdr->e_shnum * ehdr->e_shentsize;
  Elf32_Off     elf_map_size  = ehdr->e_shoff + sht_size;
  Elf32_Shdr  * p_shdr;

  Elf32_Shdr  * p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr->e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;
  Elf32_Shdr  * p_dynsym     = NULL;
  Elf32_Addr    base         = _get_base_32(ehdr, elf_map);

  if (base != UINT32_MAX) {
    log_d("Base @ %p", base);

    for (Elf32_Off sh_off = ehdr->e_shoff; \
      sh_off < elf_map_size; \
      sh_off += ehdr->e_shentsize \
    ) {
      p_shdr = (Elf32_Shdr *) (elf_map + sh_off);

      if (
        p_shdr->sh_type == SHT_DYNSYM && \
        strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0
      ) {
        p_dynsym = p_shdr;
      }
      // NOTE: This might be required if the Python version must be retrieved
      //       from the RO data section
      // else if (
      //   p_shdr->sh_type == SHT_PROGBITS &&
      //   strcmp(sh_name_base + p_shdr->sh_name, ".rodata") == 0
      // ) {
      //   self->map.rodata.base = (void *) p_shdr->sh_offset;
      //   self->map.rodata.size = p_shdr->sh_size;
      //   map_flag |= RODATA_MAP;
      // }
    }

    if (p_dynsym != NULL) {
      if (p_dynsym->sh_offset != 0) {
        Elf32_Shdr * p_strtabsh = (Elf32_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

        // Search for dynamic symbols
        for (Elf32_Off tab_off = p_dynsym->sh_offset; \
          tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
          tab_off += p_dynsym->sh_entsize
        ) {
          Elf32_Sym * sym      = (Elf32_Sym *) (elf_map + tab_off);
          char      * sym_name = (char *) (elf_map + p_strtabsh->sh_offset + sym->st_name);
          void      * value    = self->map.elf.base + (sym->st_value - base);
          if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= SYMBOLS)
            break;
        }
      }
    }
  }

  if (!symbols) {
    log_e("ELF binary has no Python symbols");
    FAIL;
  }

  SUCCESS;
} /* _py_proc__analyze_elf32 */


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf(py_proc_t * self, char * path) {
  int fd = open (path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary file %s", path);
    FAIL;
  }

  void        * binary_map  = MAP_FAILED;
  size_t        binary_size = 0;
  struct stat   s;

  with_resources;

  if (fstat(fd, &s) == -1) {
    log_ie("Cannot determine size of binary file");
    NOK;
  }

  binary_size = s.st_size;

  binary_map = mmap(0, binary_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (binary_map == MAP_FAILED) {
    log_ie("Cannot map binary file to memory");
    NOK;
  }

  Elf64_Ehdr * ehdr = binary_map;
  log_t("Analysing ELF");

  if (ehdr->e_shoff == 0 || ehdr->e_shnum < 2 || memcmp(ehdr->e_ident, ELFMAG, SELFMAG)) {
    log_d("Bad ELF magic");
    NOK;
  }

  // Dispatch
  switch (ehdr->e_ident[EI_CLASS]) {
  case ELFCLASS64:
    log_d("64-bit ELF detected");
    retval = _py_proc__analyze_elf64(self, binary_map);
    break;

  case ELFCLASS32:
    retval = _py_proc__analyze_elf32(self, binary_map);
    break;

  default:
    log_e("Invalid ELF class");
    NOK;
  }

release:
  if (binary_map != MAP_FAILED) munmap(binary_map, binary_size);
  if (fd != -1) close(fd);

  released;
} /* _py_proc__analyze_elf */


// ----------------------------------------------------------------------------
static int
_elf_is_executable(char * object_file) {
  int fd   = open(object_file, O_RDONLY);
  if (fd == -1)
    return FALSE;

  Elf64_Ehdr * ehdr = (Elf64_Ehdr *) mmap(NULL, sizeof(Elf64_Ehdr), PROT_READ, MAP_SHARED, fd, 0);

  int is_exec = ehdr->e_type == ET_EXEC;

  munmap(ehdr, sizeof(Elf64_Ehdr));
  close(fd);

  return is_exec;
} /* _elf_is_executable */


// ----------------------------------------------------------------------------
static int
_py_proc__parse_maps_file(py_proc_t * self) {
  char      file_name[32];
  FILE    * fp        = NULL;
  char    * line      = NULL;
  size_t    len       = 0;
  int       maps_flag = 0;

  sprintf(file_name, "/proc/%d/maps", self->pid);
  fp = fopen(file_name, "r");
  if (fp == NULL) {
    switch (errno) {
    case EACCES:  // Needs elevated privileges
      set_error(EPROCPERM);
      break;
    case ENOENT:  // Invalid pid
      set_error(EPROCNPID);
      break;
    default:
      set_error(EPROCVM);
    }
    FAIL;
  }

  self->min_raddr = (void *) -1;
  self->max_raddr = NULL;

  sfree(self->bin_path);
  sfree(self->lib_path);

  char prev_path[1024] = "\0";
  while (getline(&line, &len, fp) != -1) {
    ssize_t lower, upper;
    char    pathname[1024];

    int field_count = sscanf(line, "%lx-%lx %*s %*x %*x:%*x %*x %s\n",
      &lower, &upper, // Map bounds
      pathname        // Binary path
    ) - 3; // We expect between 3 and 4 matches.
    if (field_count >= 0) {
      if (field_count == 0 || strstr(pathname, "[v") == NULL) {
        // Skip meaningless addresses like [vsyscall] which would give
        // ridiculous values.
        if ((void *) lower < self->min_raddr) self->min_raddr = (void *) lower;
        if ((void *) upper > self->max_raddr) self->max_raddr = (void *) upper;
      }

      if ((maps_flag & HEAP_MAP) == 0 && strstr(line, "[heap]\n") != NULL) {
        self->map.heap.base = (void *) lower;
        self->map.heap.size = upper - lower;

        maps_flag |= HEAP_MAP;

        log_d("HEAP bounds %lx-%lx", lower, upper);
        continue;
      }

      // if (strstr(line, "python") == NULL)
      // NOTE: The python binary might have a name that doesn't contain python
      //       but would still be valid. In case of future issues, this
      //       should be changed so that the binary on the first line is
      //       checked for, e.g., knownw symbols to determine whether it is a
      //       valid binary that Austin can handle.
        // continue;

      // Check if it is an executable. Only bother if the size is above the
      // MB threshold. Anything smaller is probably not a useful binary.
      if (pathname[0] == '[')
        continue;

      if (strcmp(pathname, prev_path) == 0) // Avoid analysing a binary multiple times
        continue;

      strncpy(prev_path, pathname, sizeof(prev_path));

      ssize_t file_size = _file_size(pathname);
      if (_elf_is_executable(pathname)) {
        if (self->bin_path != NULL || (file_size < (1 << 20)))
          continue;

        self->bin_path = strndup(pathname, strlen(pathname));

        self->map.elf.base = (void *) lower;
        self->map.elf.size = upper - lower;

        if (fail(_py_proc__analyze_elf(self, self->bin_path))) {
          log_d("Possibly invalid Python binary at %p: %s", self->map.elf.base, self->bin_path);
          sfree(self->bin_path);
          self->bin_path = NULL;
        }
        else
          log_d("Candidate binary: %s (size %d KB)", pathname, file_size >> 10);

        continue;
      } else {
        if (self->bin_path != NULL || self->lib_path != NULL || (file_size < (1 << 20)))
          continue;

        self->lib_path = strndup(pathname, strlen(pathname));

        self->map.elf.base = (void *) lower;
        self->map.elf.size = upper - lower;

        if (fail(_py_proc__analyze_elf(self, self->lib_path))) {
          log_d("Possibly invalid Python library: %s", self->lib_path);
          sfree(self->lib_path);
          self->lib_path = NULL;
        }
        else
          log_d("Candidate library: %s (size %d KB)", pathname, file_size >> 10);
      }
    }
  }

  sfree(line);
  fclose(fp);

  return (
    (self->bin_path == NULL && self->lib_path == NULL) ||
    maps_flag != HEAP_MAP
  );
} /* _py_proc__parse_maps_file */


// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t * self) {
  FILE * statm = fopen(self->extra->statm_file, "rb");
  if (statm == NULL) {
    set_error(EPROCVM);
    return -1;
  }

  ssize_t size, resident;
  if (fscanf(statm, "%ld %ld", &size, &resident) != 2)
    return -1;

  fclose(statm);

  return resident * self->extra->page_size;
} /* _py_proc__get_resident_memory */


#ifdef NATIVE
// ----------------------------------------------------------------------------
static int
_py_proc__dump_maps(py_proc_t * self) {
  char      file_name[32];
  FILE    * fp        = NULL;
  char    * line      = NULL;
  size_t    len       = 0;

  sprintf(file_name, "/proc/%d/maps", self->pid);
  fp = fopen(file_name, "r");
  if (fp == NULL) {
    switch (errno) {
    case EACCES:  // Needs elevated privileges
      set_error(EPROCPERM);
      break;
    case ENOENT:  // Invalid pid
      set_error(EPROCNPID);
      break;
    default:
      set_error(EPROCVM);
    }
    FAIL;
  }

  while (getline(&line, &len, fp) != -1) {
    ssize_t lower, upper;
    char    pathname[1024];

    if (sscanf(line, "%lx-%lx %*s %*x %*x:%*x %*x %s\n",
      &lower, &upper, // Map bounds
      pathname        // Binary path
    ) == 3 && pathname[0] != '[') {
      fprintf(pargs.output_file, "# map: %lx-%lx %s\n", lower, upper, pathname);
    }
  }

  sfree(line);
  fclose(fp);

  SUCCESS;
} /* _py_proc__dump_maps */
#endif


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (
   !isvalid(self)
  ||fail   (_py_proc__parse_maps_file(self))
  ) FAIL;

  self->extra->page_size = getpagesize();
  log_d("Page size: %ld", self->extra->page_size);

  sprintf(self->extra->statm_file, "/proc/%d/statm", self->pid);

  self->last_resident_memory = _py_proc__get_resident_memory(self);

  #ifdef NATIVE
  _py_proc__dump_maps(self);
  #endif

  SUCCESS;
} /* _py_proc__init */


#endif
