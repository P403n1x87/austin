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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../dict.h"
#include "../py_proc.h"


#define CHECK_HEAP
#define DEREF_SYM


#define BIN_MAP                  (1 << 0)
#define DYNSYM_MAP               (1 << 1)
#define RODATA_MAP               (1 << 2)
#define HEAP_MAP                 (1 << 3)
#define BSS_MAP                  (1 << 4)


#define _py_proc__get_elf_type(self, offset, dt) (py_proc__memcpy(self, self->map.elf.base + offset, sizeof(dt), &dt))

// Get the offset of the ith section header
#define ELF_SH_OFF(ehdr, i)            (ehdr.e_shoff + i * ehdr.e_shentsize)

#define get_bounds(line, a, b)         (sscanf(line, "%lx-%lx", &a, &b))

union {
  Elf32_Ehdr v32;
  Elf64_Ehdr v64;
} ehdr_v;


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf64(py_proc_t * self) {
  Elf64_Ehdr ehdr = ehdr_v.v64;

  // Section header must be read from binary as it is not loaded into memory
  Elf64_Xword   sht_size      = ehdr.e_shnum * ehdr.e_shentsize;
  Elf64_Off     elf_map_size  = ehdr.e_shoff + sht_size;
  int           fd            = open(self->bin_path, O_RDONLY);
  void        * elf_map       = mmap(NULL, elf_map_size, PROT_READ, MAP_SHARED, fd, 0);
  int           map_flag      = 0;
  Elf64_Shdr  * p_shdr;

  Elf64_Shdr  * p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr.e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;
  Elf64_Shdr  * p_dynsym     = NULL;
  Elf64_Addr    base         = 0;

  if (ehdr.e_phnum) {
    Elf64_Phdr * phdr = (Elf64_Phdr *) (elf_map + ehdr.e_phoff);
    base = phdr->p_vaddr - phdr->p_offset;
    if (!base)
      base = (Elf64_Addr) self->map.elf.base;
    log_d("Base @ %p", base);
  }

  for (Elf64_Off sh_off = ehdr.e_shoff; \
    map_flag != DYNSYM_MAP && sh_off < elf_map_size; \
    sh_off += ehdr.e_shentsize \
  ) {
    p_shdr = (Elf64_Shdr *) (elf_map + sh_off);

    if (
      !(map_flag & DYNSYM_MAP) &&
      p_shdr->sh_type == SHT_DYNSYM && \
      strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0
    ) {
      p_dynsym = p_shdr;
      map_flag |= DYNSYM_MAP;
    }
    // NOTE: This might be required if the Python version is must be retrieved
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

  register int hit_cnt = 0;
  if (p_dynsym != NULL) {
    if (p_dynsym->sh_offset == 0)
      return 1;

    Elf64_Shdr * p_strtabsh = (Elf64_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

    // Search for dynamic symbols
    for (Elf64_Off tab_off = p_dynsym->sh_offset; \
      tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
      tab_off += p_dynsym->sh_entsize
    ) {
      Elf64_Sym * sym      = (Elf64_Sym *) (elf_map + tab_off);
      char      * sym_name = (char *) (elf_map + p_strtabsh->sh_offset + sym->st_name);
      // This leads to some good corrections, but it still fails in some cases.
      void      * value    = (void *) sym->st_value >= self->map.bss.base && (void *) sym->st_value < self->map.bss.base + self->map.bss.size \
        ? (void *) sym->st_value \
        : (void *) base + (sym->st_value);
      if ((hit_cnt = _py_proc__check_sym(self, sym_name, value)))
        break;
    }
  }

  munmap(elf_map, elf_map_size);
  close(fd);

  return 1 - hit_cnt;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf32(py_proc_t * self) {
  Elf32_Ehdr ehdr = ehdr_v.v32;

  // Section header must be read from binary as it is not loaded into memory
  Elf32_Xword   sht_size      = ehdr.e_shnum * ehdr.e_shentsize;
  Elf32_Off     elf_map_size  = ehdr.e_shoff + sht_size;
  int           fd            = open(self->bin_path, O_RDONLY);
  void        * elf_map       = mmap(NULL, elf_map_size, PROT_READ, MAP_SHARED, fd, 0);
  int           map_flag      = 0;
  Elf32_Shdr  * p_shdr;

  Elf32_Shdr  * p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr.e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;
  Elf32_Shdr  * p_dynsym     = NULL;
  Elf32_Addr    base         = 0;

  if (ehdr.e_phnum) {
    Elf32_Phdr * phdr = (Elf32_Phdr *) (elf_map + ehdr.e_phoff);
    base = phdr->p_vaddr - phdr->p_offset;
    if (!base)
      base = (Elf32_Addr) self->map.elf.base;
    log_d("Base @ %p", base);
  }

  for (Elf32_Off sh_off = ehdr.e_shoff; \
    map_flag != DYNSYM_MAP && sh_off < elf_map_size; \
    sh_off += ehdr.e_shentsize \
  ) {
    p_shdr = (Elf32_Shdr *) (elf_map + sh_off);

    if (
      !(map_flag & DYNSYM_MAP) &&
      p_shdr->sh_type == SHT_DYNSYM && \
      strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0
    ) {
      p_dynsym = p_shdr;
      map_flag |= DYNSYM_MAP;
    }
    // NOTE: This might be required if the Python version is must be retrieved
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

  register int hit_cnt = 0;
  if (p_dynsym != NULL) {
    if (p_dynsym->sh_offset == 0)
      return 1;

    Elf32_Shdr * p_strtabsh = (Elf32_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

    // Search for dynamic symbols
    for (Elf32_Off tab_off = p_dynsym->sh_offset; \
      tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
      tab_off += p_dynsym->sh_entsize
    ) {
      Elf32_Sym * sym      = (Elf32_Sym *) (elf_map + tab_off);
      char      * sym_name = (char *) (elf_map + p_strtabsh->sh_offset + sym->st_name);
      // This leads to some good corrections, but it still fails in some cases.
      void      * value    = (void *) sym->st_value >= self->map.bss.base && (void *) sym->st_value < self->map.bss.base + self->map.bss.size \
        ? (void *) sym->st_value \
        : (void *) base + (sym->st_value);
      if ((hit_cnt = _py_proc__check_sym(self, sym_name, value)))
        break;
    }
  }

  munmap(elf_map, elf_map_size);
  close(fd);

  return 1 - hit_cnt;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf(py_proc_t * self) {
  Elf64_Ehdr ehdr = ehdr_v.v64;

  if (
    _py_proc__get_elf_type(self, 0, ehdr_v) ||
    ehdr.e_shoff == 0 ||
    ehdr.e_shnum < 2 ||
    memcmp(ehdr.e_ident, ELFMAG, SELFMAG)
  ) return 1;

  // Dispatch
  switch (ehdr.e_ident[EI_CLASS]) {
  case ELFCLASS32:
    return _py_proc__analyze_elf32(self);

  case ELFCLASS64:
    return _py_proc__analyze_elf64(self);

  default:
    return 1;
  }
}


// ----------------------------------------------------------------------------
static int
_py_proc__parse_maps_file(py_proc_t * self) {
  char      file_name[32];
  FILE    * fp        = NULL;
  char    * line      = NULL;
  size_t    len       = 0;
  int       maps_flag = 0;

  if (self->maps_loaded)
    return 0;

  sprintf(file_name, "/proc/%d/maps", self->pid);
  fp = fopen(file_name, "r");
  if (fp == NULL)
    error = EPROCVM;

  else {
    ssize_t a, b;  // VM map bounds
    char * needle;
    while (maps_flag != (HEAP_MAP | BSS_MAP) && getline(&line, &len, fp) != -1) {
      if (self->bin_path == NULL) {
        // Store binary file name
        // TODO: Extend to deal with libpython.so
        if ((needle = strstr(line, "python")) == NULL)
          break;

        while (*((char *) --needle) != ' ');
        int len = strlen(++needle);
        if (self->bin_path != NULL)
          free(self->bin_path);
        self->bin_path = (char *) malloc(sizeof(char) * len + 1);
        if (self->bin_path == NULL)
          error = EPROCVM;
        else {
          strcpy(self->bin_path, needle);
          if (self->bin_path[len-1] == '\n')
            self->bin_path[len-1] = 0;

          get_bounds(line, a, b);
          self->map.elf.base = (void *) a;
          self->map.elf.size = b - a;
        }
      }
      else {
        if ((needle = strstr(line, "[heap]\n")) != NULL) {
          if ((maps_flag & HEAP_MAP) == 0) {
            get_bounds(line, a, b);
            self->map.heap.base = (void *) a;
            self->map.heap.size = b - a;

            maps_flag |= HEAP_MAP;

            log_d("HEAP bounds: %s", line);
          }
        }
        else if ((maps_flag & BSS_MAP) == 0 && (line[strlen(line)-2] == ' ')) {
          get_bounds(line, a, b);
          self->map.bss.base = (void *) a;
          self->map.bss.size = b - a;

          maps_flag |= BSS_MAP;

          log_d("BSS bounds: %s", line);
        }
      }
    }

    fclose(fp);
    if (line != NULL) {
      free(line);
    }
  }

  if (error & EPROC) log_error();

  self->maps_loaded = maps_flag == (HEAP_MAP | BSS_MAP);

  return maps_flag != (HEAP_MAP | BSS_MAP);
}


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (
    self == NULL ||
    _py_proc__parse_maps_file(self) ||
    _py_proc__analyze_elf(self)
  ) return 1;

  return 0;
}


#endif
