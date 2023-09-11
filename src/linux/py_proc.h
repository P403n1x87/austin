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

#include "common.h"
#include "futils.h"
#include "../mem.h"
#include "../resources.h"

#ifdef NATIVE
#include "../argparse.h"
#include "../cache.h"
#endif
#include "../py_string.h"
#include "../hints.h"
#include "../py_proc.h"
#include "../version.h"


#define BIN_MAP                  (1 << 0)
#define DYNSYM_MAP               (1 << 1)
#define RODATA_MAP               (1 << 2)
#define BSS_MAP                  (1 << 3)


// Get the offset of the ith section header
#define ELF_SH_OFF(ehdr, i) /* as */ (ehdr->e_shoff + i * ehdr->e_shentsize)


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

  if (fail(stat(file, &statbuf)))
    return -1;

  return statbuf.st_size;
}


/*[[[cog
from pathlib import Path
analyze_elf = Path("src/linux/analyze_elf.h").read_text()
print(analyze_elf)
print(analyze_elf.replace("64", "32"))
]]]*/
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
_py_proc__analyze_elf64(py_proc_t * self, void * elf_map, void * elf_base) {
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

  void         * bss_base    = NULL;
  size_t         bss_size    = 0;

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
      else if (strcmp(sh_name_base + p_shdr->sh_name, ".bss") == 0) {
        bss_base = elf_base + (p_shdr->sh_addr - base);
        bss_size = p_shdr->sh_size;
      }
      else if (strcmp(sh_name_base + p_shdr->sh_name, ".PyRuntime") == 0) {
        self->map.runtime.base = elf_base + (p_shdr->sh_addr - base);
        self->map.runtime.size = p_shdr->sh_size;
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
          void      * value    = elf_base + (sym->st_value - base);
          if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= DYNSYM_COUNT) {
            // We have found all the symbols. No need to look further
            break;
          }
        }
      }
    }
  }

  if (symbols < DYNSYM_MANDATORY) {
    log_e("ELF binary has not all the mandatory Python symbols");
    set_error(ESYM);
    FAIL;
  }

  // Communicate BSS data back to the caller
  self->map.bss.base = bss_base;
  self->map.bss.size = bss_size;
  log_d("BSS @ %p (size %x, offset %x)", self->map.bss.base, self->map.bss.size, self->map.bss.base - elf_base);

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
_py_proc__analyze_elf32(py_proc_t * self, void * elf_map, void * elf_base) {
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

  void         * bss_base    = NULL;
  size_t         bss_size    = 0;

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
      else if (strcmp(sh_name_base + p_shdr->sh_name, ".bss") == 0) {
        bss_base = elf_base + (p_shdr->sh_addr - base);
        bss_size = p_shdr->sh_size;
      }
      else if (strcmp(sh_name_base + p_shdr->sh_name, ".PyRuntime") == 0) {
        self->map.runtime.base = elf_base + (p_shdr->sh_addr - base);
        self->map.runtime.size = p_shdr->sh_size;
      }
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
          void      * value    = elf_base + (sym->st_value - base);
          if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= DYNSYM_COUNT) {
            // We have found all the symbols. No need to look further
            break;
          }
        }
      }
    }
  }

  if (symbols < DYNSYM_MANDATORY) {
    log_e("ELF binary has not all the mandatory Python symbols");
    set_error(ESYM);
    FAIL;
  }

  // Communicate BSS data back to the caller
  self->map.bss.base = bss_base;
  self->map.bss.size = bss_size;
  log_d("BSS @ %p (size %x, offset %x)", self->map.bss.base, self->map.bss.size, self->map.bss.base - elf_base);

  SUCCESS;
} /* _py_proc__analyze_elf32 */

//[[[end]]]

// ----------------------------------------------------------------------------
static int
_elf_check(Elf64_Ehdr * ehdr) {
  return (ehdr->e_shoff == 0 || ehdr->e_shnum < 2 || memcmp(ehdr->e_ident, ELFMAG, SELFMAG));
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf(py_proc_t * self, char * path, void * elf_base) {
  cu_fd fd = open(path, O_RDONLY);
  if (fd == -1) {
    log_e("Cannot open binary file %s", path);
    set_error(EPROC);
    FAIL;
  }

  cu_map_t    * binary_map  = NULL;
  size_t        binary_size = 0;
  struct stat   s;

  if (fstat(fd, &s) == -1) {
    log_ie("Cannot determine size of binary file");
    set_error(EPROC);
    FAIL;
  }

  binary_size = s.st_size;

  binary_map = map_new(fd, binary_size, MAP_PRIVATE);
  if (!isvalid(binary_map)) {
    log_ie("Cannot map binary file to memory");
    set_error(EPROC);
    FAIL;
  }

  Elf64_Ehdr * ehdr = binary_map->addr;
  log_t("Analysing ELF");

  if (fail(_elf_check(ehdr))) {
    log_e("Bad ELF header");
    set_error(EPROC);
    FAIL;
  }

  // Dispatch
  switch (ehdr->e_ident[EI_CLASS]) {
  case ELFCLASS64:
    log_d("%s is 64-bit ELF", path);
    return _py_proc__analyze_elf64(self, binary_map->addr, elf_base);

  case ELFCLASS32:
    log_d("%s is 32-bit ELF", path);
    return _py_proc__analyze_elf32(self, binary_map->addr, elf_base);

  default:
    log_e("%s has invalid ELF class", path);
    set_error(EPROC);
    FAIL;
  }
} /* _py_proc__analyze_elf */


// ----------------------------------------------------------------------------
static int
_py_proc__parse_maps_file(py_proc_t * self) {
  char      file_name[32];
  cu_FILE * fp          = NULL;
  cu_char * line        = NULL;
  cu_char * prev_path   = NULL;
  cu_char * needle_path = NULL;
  size_t    len         = 0;
  int       maps_flag   = 0;

  struct vm_map * map = NULL;

  fp = _procfs(self->pid, "maps");
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

  // Save the file hash and the modified time. We'll use them to detect if the
  // content has changed since the last time we read it.
  crc32_t file_hash = fhash(fp);
  long modified_time = fmtime_ns(fp);

  sfree(self->bin_path);
  sfree(self->lib_path);

  self->map.exe.base = NULL;
  self->map.exe.size = 0;

  sprintf(file_name, "/proc/%d/exe", self->pid);

  cu_void * pd_mem = calloc(1, sizeof(struct proc_desc));
  if (!isvalid(pd_mem)) {
    log_ie("Cannot allocate memory for proc_desc");
    set_error(EPROC);
    FAIL;
  }
  struct proc_desc * pd = pd_mem;

  if (readlink(file_name, pd->exe_path, sizeof(pd->exe_path)) == -1) {
    log_e("Cannot readlink %s", file_name);
    set_error(EPROC);
    FAIL;  // cppcheck-suppress [resourceLeak]
  }
  if (strcmp(pd->exe_path + (strlen(pd->exe_path) - 10), " (deleted)") == 0) {
    pd->exe_path[strlen(pd->exe_path) - 10] = '\0';
  }
  log_d("Executable path: %s", pd->exe_path);

  while (getline(&line, &len, fp) != -1) {
    ssize_t lower, upper;
    char    pathname[1024] = {0};
    char    perms[5] = {0};

    int field_count = sscanf(line, ADDR_FMT "-" ADDR_FMT " %s %*x %*x:%*x %*x %s\n",
      &lower, &upper, // Map bounds
      perms,          // Permissions
      pathname        // Binary path
    ) - 3; // We expect between 3 and 4 matches.

    if (
      field_count == 0 && isvalid(map) && !isvalid(map->bss_base)
      && strcmp(perms, "rw-p") == 0
    ) {
      // The BSS section is not mapped from a file and has rw permissions.
      // We find that the map reported by proc fs is rounded to the next page
      // boundary, so we need to adjust the values. We might slide into the data
      // section, but that should be readable anyway.
      size_t page_size = getpagesize();
      map->bss_base = (void *) lower - page_size;
      map->bss_size = upper - lower + page_size;
      log_d("BSS section inferred from VM maps for %s: %lx-%lx", map->path, lower, upper);
    }

    if (field_count <= 0)
      continue;

    if (
      isvalid(map) && !isvalid(self->map.runtime.base)
      && strcmp(perms, "rw-p") == 0 && strcmp(map->path, pathname) == 0
    ) {
      // This is likely the PyRuntime section.
      size_t page_size = getpagesize();
      self->map.runtime.base = (void *) lower - page_size;
      self->map.runtime.size = upper - lower + page_size;
      log_d("PyRuntime section inferred from VM maps for %s: %lx-%lx", map->path, lower, upper);
    }

    if (pathname[0] == '[')
      continue;

    if (isvalid(prev_path) && strcmp(pathname, prev_path) == 0) { // Avoid analysing a binary multiple times
      continue;
    }
    
    sfree(prev_path);
    prev_path = strndup(pathname, strlen(pathname));
    if (!isvalid(prev_path)) {
      log_ie("Cannot duplicate path name");
      set_error(EPROC);
      FAIL;
    }

    // The first memory map of the executable
    if (!isvalid(pd->maps[MAP_BIN].path) && strcmp(pd->exe_path, pathname) == 0) {
      map = &(pd->maps[MAP_BIN]);
      map->path = strndup(pathname, strlen(pathname));
      if (!isvalid(map->path)) {
        log_ie("Cannot duplicate path name");
        set_error(EPROC);
        FAIL;
      }
      map->file_size = _file_size(pathname);
      map->base = (void *) lower;
      map->size = upper - lower;
      map->has_symbols = success(_py_proc__analyze_elf(self, pathname, (void *) lower));
      if (map->has_symbols) {
        map->bss_base = self->map.bss.base;
        map->bss_size = self->map.bss.size;
      }
      log_d("Binary map: %s (symbols %d)", map->path, map->has_symbols);
      continue;
    }

    // The first memory map of the shared library (if any)
    char * needle = strstr(pathname, "libpython");
    if (!isvalid(pd->maps[MAP_LIBSYM].path) && isvalid(needle)) {
      int has_symbols = success(_py_proc__analyze_elf(self, pathname, (void *) lower));
      if (has_symbols) {
        map = &(pd->maps[MAP_LIBSYM]);
        map->path = strndup(pathname, strlen(pathname));
        if (!isvalid(map->path)) {
          log_ie("Cannot duplicate path name");
          set_error(EPROC);
          FAIL;
        }
        map->file_size = _file_size(pathname);
        map->base = (void *) lower;
        map->size = upper - lower;
        map->has_symbols = TRUE;
        map->bss_base = self->map.bss.base;
        map->bss_size = self->map.bss.size;

        log_d("Library map: %s (with symbols)", map->path);

        continue;
      }
      
      // The first memory map of a binary that contains "pythonX.Y" in its name
      if (!isvalid(pd->maps[MAP_LIBNEEDLE].path)) {
        if (isvalid(needle)) {
          unsigned int v;
          if (sscanf(needle, "libpython%u.%u", &v, &v) == 2) {
            map = &(pd->maps[MAP_LIBNEEDLE]);
            map->path = needle_path = strndup(pathname, strlen(pathname));
            if (!isvalid(map->path)) {
              log_ie("Cannot duplicate path name");
              set_error(EPROC);
              FAIL;
            }
            map->file_size = _file_size(pathname);
            map->base = (void *) lower;
            map->size = upper - lower;
            map->has_symbols = FALSE;
            log_d("Library map: %s (needle)", map->path);
            continue;
          }
        }
      }
    }
  }

  // If the library map is not valid, use the needle map
  if (!isvalid(pd->maps[MAP_LIBSYM].path)) {
    pd->maps[MAP_LIBSYM] = pd->maps[MAP_LIBNEEDLE];
    pd->maps[MAP_LIBNEEDLE].path = needle_path = NULL;
  }

  // Work out paths
  self->bin_path = pd->maps[MAP_BIN].path;
  self->lib_path = pd->maps[MAP_LIBSYM].path;

  // Work out binary map
  for (int i = 0; i < MAP_COUNT; i++) {
    map = &(pd->maps[i]);
    if (map->has_symbols) {
      self->map.exe.base = map->base;
      self->map.exe.size = map->size;
      maps_flag |= BIN_MAP;
      self->sym_loaded = TRUE;
      break;
    }
  }

  if (!(maps_flag & BIN_MAP) && !isvalid(pd->maps[MAP_LIBNEEDLE].path)) {
    // We don't have symbols and we don't have a needle path so it's quite
    // unlikely that we can work out a Python version in this case.
    if (isvalid(pd->maps[MAP_BIN].path) && strstr(pd->maps[MAP_BIN].path, "python")) {
      log_d("No symbols but binary seems to be Python.");
      maps_flag |= BIN_MAP;
    } else {
      log_d("No symbols and no needle path. Giving up.");
      FAIL;
    }
  }

  // Work out BSS map
  int map_index = isvalid(pd->maps[MAP_LIBSYM].path) ? MAP_LIBSYM : MAP_BIN;
  self->map.bss.base = pd->maps[map_index].bss_base;
  self->map.bss.size = pd->maps[map_index].bss_size;
  
  if (!isvalid(self->map.bss.base)) {
    log_e("Cannot find valid BSS map");
    set_error(EPROCVM);
    FAIL;
  }

  if (!(maps_flag & (BIN_MAP))) {
    log_e("No usable Python binary found");
    set_error(EPROC);
    FAIL;
  }

  if (!(modified_time == fmtime_ns(fp) && file_hash == fhash(fp))) {
    log_e("VM maps file has changed since last read");
    set_error(EPROCVM);
    FAIL;
  }

  log_d("BSS map %d from %s @ %p", map_index, pd->maps[map_index].path, self->map.bss.base);
  log_d("VM maps parsing result: bin=%s lib=%s flags=%d", self->bin_path, self->lib_path, maps_flag);

  SUCCESS;
} /* _py_proc__parse_maps_file */


// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t * self) {
  FILE * statm = fopen(self->extra->statm_file, "rb");
  if (statm == NULL) {
    set_error(EPROCVM);
    return -1;
  }

  int ret = 0;

  ssize_t size, resident;
  if (fscanf(statm, SIZE_FMT " " SIZE_FMT, &size, &resident) != 2)
    ret = -1;

  fclose(statm);

  return ret ? ret : resident * self->extra->page_size;
} /* _py_proc__get_resident_memory */


#ifdef NATIVE
// ----------------------------------------------------------------------------
char         pathname[1024];
char         prevpathname[1024];
vm_range_t * ranges[256];

static int
_py_proc__get_vm_maps(py_proc_t * self) {
  cu_FILE         * fp    = NULL;
  cu_char         * line  = NULL;
  size_t            len   = 0;
  vm_range_tree_t * tree  = NULL;
  hash_table_t    * table = NULL;
  
  if (pargs.where) {
    tree  = vm_range_tree_new();
    table = hash_table_new(256);
    
    vm_range_tree__destroy(self->maps_tree);
    hash_table__destroy(self->base_table);
    
    self->maps_tree = tree; 
    self->base_table = table;
  }

  fp = _procfs(self->pid, "maps");
  if (!isvalid(fp)) {
    set_error(EPROC);
    FAIL;
  }

  log_d("Rebuilding vm ranges tree");

  int    nrange  = 0;
  while (getline(&line, &len, fp) != -1 && nrange < 256) {
    ssize_t lower, upper;

    if (sscanf(line, ADDR_FMT "-" ADDR_FMT " %*s %*x %*x:%*x %*x %s\n",
      &lower, &upper, // Map bounds
      pathname        // Binary path
    ) == 3 && pathname[0] != '[') {
      if (pargs.where) {
        if (strcmp(pathname, prevpathname)) {
          ranges[nrange++] = vm_range_new(lower, upper, strdup(pathname));
          key_dt key = string__hash(pathname);
          if (!isvalid(hash_table__get(table, key)))
            hash_table__set(table, key, (value_t) lower);
          strcpy(prevpathname, pathname);
        } else
          ranges[nrange-1]->hi = upper;
      }
      else
        // We print the maps instead so that we can resolve them later and use
        // the CPU more efficiently to collect samples.
        emit_metadata("map", ADDR_FMT "-" ADDR_FMT " %s", lower, upper, pathname);
    }
  }

  for (int i = 0; i < nrange; i++)
    vm_range_tree__add(tree, (vm_range_t *) ranges[i]); 

  SUCCESS;
} /* _py_proc__get_vm_maps */
#endif


// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t * self) {
  if (!isvalid(self) || fail(_py_proc__parse_maps_file(self))) {
    set_error(EPROC);
    FAIL;
  }

  self->extra->page_size = getpagesize();
  log_d("Page size: %u", self->extra->page_size);

  sprintf(self->extra->statm_file, "/proc/%d/statm", self->pid);

  self->last_resident_memory = _py_proc__get_resident_memory(self);

  #ifdef NATIVE
  _py_proc__get_vm_maps(self);
  #endif

  SUCCESS;
} /* _py_proc__init */


// ----------------------------------------------------------------------------
pid_t
_get_nspid(pid_t pid) {
  cu_char * line  = NULL;
  size_t    len   = 0;
  pid_t     nspid = 0;
  pid_t     this  = 0;

  cu_FILE * status = _procfs(pid, "status");
  if (!isvalid(status)) {
    log_e("Cannot get namespace PID for %d", pid);
    return 0;
  }

  while (getline(&line, &len, status) != -1) {
    if (sscanf(line, "NSpid:\t%d\t%d", &this, &nspid) == 2 && this == pid) {
      break;
    }
  }

  log_d("NS PID for %d: %d", pid, nspid);

  return nspid;
}


// Support for CPU time on Linux. We need to retrieve the TID from the struct
// pthread pointed to by the native thread ID stored by Python. We do not have
// the definition of the structure, so we need to "guess" the offset of the tid
// field within struct pthread.

// ----------------------------------------------------------------------------
static int
_infer_tid_field_offset(py_thread_t * py_thread) {
  if (fail(read_pthread_t(py_thread->proc, (void *) py_thread->tid))) {
    log_d("> Cannot copy pthread_t structure (pid: %u)", py_thread->raddr.pref);
    set_error(EMMAP);
    FAIL;
  }

  log_d("pthread_t at %p", py_thread->tid);

  // If the target process is in a different PID namespace, we need to get its
  // other PID to be able to determine the offset of the TID field.
  pid_t nspid = _get_nspid(py_thread->raddr.pref);

  for (register int i = 0; i < PTHREAD_BUFFER_ITEMS; i++) {
    if (
      py_thread->raddr.pref == py_thread->proc->extra->_pthread_buffer[i]
      || (nspid && nspid == py_thread->proc->extra->_pthread_buffer[i])
    ) {
      log_d("TID field offset: %d", i);
      py_thread->proc->extra->pthread_tid_offset = i;
      SUCCESS;
    }
  }

  // Fall-back to smaller steps if we failed
  for (register int i = 0; i < PTHREAD_BUFFER_ITEMS * (sizeof(uintptr_t) / sizeof(pid_t)); i++) {
    if (
      py_thread->raddr.pref == (pid_t) ((pid_t *) py_thread->proc->extra->_pthread_buffer)[i]
      || (nspid && nspid == (pid_t) ((pid_t *) py_thread->proc->extra->_pthread_buffer)[i])
    ) {
      log_d("TID field offset (from fall-back): %d", i);
      py_thread->proc->extra->pthread_tid_offset = -i;
      SUCCESS;
    }
  }

  set_error(ETHREAD);
  FAIL;
}

#endif
