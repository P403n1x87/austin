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

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>   /* LINUX */
#include <sys/wait.h>
#include <unistd.h>

#include "dict.h"
#include "error.h"
#include "logging.h"
#include "mem.h"

#include "py_proc.h"
#include "py_thread.h"


// ---- PRIVATE ---------------------------------------------------------------

#define INIT_RETRY_SLEEP             100  /* usecs */
#define INIT_RETRY_CNT               100  /* Retry for 10 ms before giving up. */

#define BIN_MAP                  (1 << 0)
#define DYNSYM_MAP               (1 << 1)
#define RODATA_MAP               (1 << 2)
#define HEAP_MAP                 (1 << 3)


#define _py_proc__get_elf_type(self, offset, dt) (py_proc__memcpy(self, self->map.elf.base + offset, sizeof(dt), &dt))


#define DYNSYM_COUNT                   1

static const char * _dynsym_array[DYNSYM_COUNT] = {
  "_PyThreadState_Current"
};

static long _dynsym_hash_array[DYNSYM_COUNT] = {
  0
};


// Get the offset of the ith section header
#define ELF_SH_OFF(ehdr, i) (ehdr.e_shoff + i * ehdr.e_shentsize)
#define get_bounds(line, a, b) (sscanf(line, "%lx-%lx", &a, &b))


// ----------------------------------------------------------------------------
static int
_py_proc__parse_maps_file(py_proc_t * self) {
  char      file_name[32];
  FILE    * fp        = NULL;
  char    * line      = NULL;
  size_t    len       = 0;

  if (self->bin_path != NULL)
    return 0;

  sprintf(file_name, "/proc/%d/maps", self->pid);

  #ifdef DEBUG
  log_d("Opening %s", file_name);
  #endif // DEBUG

  fp = fopen(file_name, "r");
  if (fp == NULL)
    error = EPROCVM;

  else {
    int maps_flag = 0;
    ssize_t a, b;  // VM map bounds
    char * needle;
    while (maps_flag != (BIN_MAP | HEAP_MAP) && getline(&line, &len, fp) != -1) {
      if ((maps_flag & BIN_MAP) == 0) {
        needle = strstr(line, "python");
        if (needle != NULL) {
          // Store binary file name
          // TODO: Extend to deal with libpython.so
          while (*((char *) --needle) != ' ');
          int len = strlen(++needle);
          if (self->bin_path != NULL)
            free(self->bin_path);
          self->bin_path = (char *) malloc(sizeof(char) * len);
          if (self->bin_path == NULL)
            error = EPROCVM;
          else {
            strcpy(self->bin_path, needle);
            if (self->bin_path[len-1] == '\n')
              self->bin_path[len-1] = 0;

            get_bounds(line, a, b);
            self->map.elf.base = (void *) a;
            self->map.elf.size = b - a;

            #ifdef DEBUG
            log_d("Python binary path: %s\n", self->bin_path);
            #endif // DEBUG

            maps_flag |= BIN_MAP;
          }
        }
      }
      if ((maps_flag & HEAP_MAP) == 0) {
        needle = strstr(line, "[heap]\n");
        if (needle != NULL) {
          get_bounds(line, a, b);
          self->map.heap.base = (void *) a;
          self->map.heap.size = b - a;

          maps_flag |= HEAP_MAP;
        }
      }
    }

    fclose(fp);
    if (line != NULL) {
      free(line);
    }
  }

  if (error & EPROC) log_error();

  return self->bin_path == NULL ? 1 : 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_version(py_proc_t * self, void * map) {
  if (self == NULL || map == NULL)
    return 0;

  int major = 0, minor = 0, patch = 0;

  // Scan the rodata section for something that looks like the Python version.
  // There are good chances this is at the very beginning of the section so
  // it shouldn't take too long to find a match. This is more reliable than
  // waiting until the version appears in the bss section at run-time.
  // NOTE: This method is not guaranteed to find a valid Python version.
  //       If this causes problems then another method is required.
  char * p_ver = (char *) map + (Elf64_Addr) self->map.rodata.base;
  for (register int i = 0; i < self->map.rodata.size; i++) {
    if (p_ver[i] == '.' && p_ver[i+1] != '.' && p_ver[i+2] == '.' && p_ver[i-2] == 0) {
      if (sscanf(p_ver + i - 1, "%d.%d.%d", &major, &minor, &patch) == 3 && (major == 2 || major == 3)) {
        #ifdef DEBUG
        log_d("Python version: %s", p_ver + i - 1, p_ver);
        #endif
        break;
      }
    }
  }

  return (major << 16) | (minor << 8) | patch;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_bin(py_proc_t * self) {
  if (self == NULL)
    return 1;

  if (self->sym_loaded)
    return 0;

  if (self->bin_path == NULL)
    _py_proc__parse_maps_file(self);

  if (self->bin_path == NULL)
    return 1;

  Elf64_Ehdr ehdr;

  if (_py_proc__get_elf_type(self, 0, ehdr) || ehdr.e_shoff == 0 || ehdr.e_shnum < 2 \
    || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' \
    || ehdr.e_ident[EI_CLASS] != ELFCLASS64 \
  ) return 1;

  // Section header must be read from binary as it is not loaded into memory
  Elf64_Xword   sht_size      = ehdr.e_shnum * ehdr.e_shentsize;
  Elf64_Off     elf_map_size  = ehdr.e_shoff + sht_size;
  int           fd            = open(self->bin_path, O_RDONLY);
  void        * elf_map       = mmap(NULL, elf_map_size, PROT_READ, MAP_SHARED, fd, 0);
  int           map_flag      = 0;
  Elf64_Shdr  * p_shdr;

  Elf64_Shdr  * p_shstrtab = elf_map + ELF_SH_OFF(ehdr, ehdr.e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;

  for (Elf64_Off sh_off = ehdr.e_shoff; \
    map_flag != (DYNSYM_MAP | RODATA_MAP) && sh_off < elf_map_size; \
    sh_off += ehdr.e_shentsize \
  ) {
    p_shdr = (Elf64_Shdr *) (elf_map + sh_off);

    if (   p_shdr->sh_type == SHT_DYNSYM \
        && strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0
    ) {
      self->map.dynsym.base = p_shdr;  // WARNING: Different semantics!
      map_flag |= DYNSYM_MAP;
    }
    else if (p_shdr->sh_type == SHT_PROGBITS \
        && strcmp(sh_name_base + p_shdr->sh_name, ".rodata") == 0
    ) {
      self->map.rodata.base = (void *) p_shdr->sh_offset;
      self->map.rodata.size = p_shdr->sh_size;
      map_flag |= RODATA_MAP;
    }
  }

  register int hit_cnt = 0;
  if (map_flag == (DYNSYM_MAP | RODATA_MAP)) {
    self->version = _py_proc__get_version(self, elf_map);
    if (self->version) {
      if ((self->version & 0x00FFFF00) != ((3 << 16) | (6 << 8))) // Version 3.6
        log_w("Unsupported Python version detected. Austin might not work as expected.");

      Elf64_Shdr * p_dynsym = self->map.dynsym.base;
      if (p_dynsym->sh_offset == 0)
        return 1;

      Elf64_Shdr * p_strtabsh = (Elf64_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

      // NOTE: This runs sub-optimally when searching for a single symbol
      // Pre-hash symbol names
      if (_dynsym_hash_array[0] == 0) {
        for (register int i = 0; i < DYNSYM_COUNT; i++)
          _dynsym_hash_array[i] = string_hash((char *) _dynsym_array[i]);
      }

      // Search for dynamic symbols
      for (Elf64_Off tab_off = p_dynsym->sh_offset; \
        hit_cnt < DYNSYM_COUNT && tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
        tab_off += p_dynsym->sh_entsize
      ) {
        Elf64_Sym * sym      = (Elf64_Sym *) (elf_map + tab_off);
        char      * sym_name = elf_map + p_strtabsh->sh_offset + sym->st_name;
        long        hash     = string_hash(sym_name);
        for (register int i = 0; i < DYNSYM_COUNT; i++) {
          if (hash == _dynsym_hash_array[i] && strcmp(sym_name, _dynsym_array[i]) == 0) {
            *(&(self->tstate_curr_raddr) + i) = (void *) sym->st_value;
            hit_cnt++;
            #ifdef DEBUG
            log_d("Symbol %s found at %p", sym_name, sym->st_value);
            #endif
          }
        }
      }
    }
  }

  munmap(elf_map, elf_map_size);
  close(fd);

  self->sym_loaded = (hit_cnt == DYNSYM_COUNT) ? 1 : 0;

  return 1 - self->sym_loaded;
}

// ----------------------------------------------------------------------------
static int
_py_proc__check_interp_state(py_proc_t * self, void * raddr) {
  PyInterpreterState is;
  PyThreadState      tstate_head;

  if (py_proc__get_type(self, raddr, is) != 0)
    return -1; // This signals that we are out of bounds.

  if (py_proc__get_type(self, is.tstate_head, tstate_head) != 0)
    return 1;

  if (tstate_head.interp != raddr || tstate_head.frame == 0)
    return 1;

  error = EOK;
  raddr_t thread_raddr = { .pid = self->pid, .addr = is.tstate_head };
  py_thread_t * thread = py_thread_new_from_raddr(&thread_raddr);
  if (thread == NULL)
    return 1;
  py_thread__destroy(thread);

  return error == EOK ? 0 : 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__get_interp_state(py_proc_t * self) {
  PyThreadState tstate_current;

  if (self == NULL)
    return 1;

  if (self->sym_loaded == 0)
    _py_proc__analyze_bin(self);

  if (self->sym_loaded == 0)
    return 1;

  if (py_proc__get_type(self, self->tstate_curr_raddr, tstate_current) != 0)
    return 1;

  // 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
  //                 anymore, but _PyThreadState_Current.prev is.
  if (tstate_current.thread_id == 0 && tstate_current.prev != 0) {
    self->tstate_curr_raddr = tstate_current.prev;
    return 1;
  }

  if (_py_proc__check_interp_state(self, tstate_current.interp))
    return 1;

  self->is_raddr = tstate_current.interp;

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__is_heap_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.heap.base == NULL)
    return 0;

  return (raddr >= self->map.heap.base && raddr < self->map.heap.base + self->map.heap.size)
    ? 1
    : 0;
}

// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  register int try_cnt = INIT_RETRY_CNT;

  while (try_cnt--) {
    usleep(INIT_RETRY_SLEEP);

    if (_py_proc__get_interp_state(self) == 0) {

      #ifdef DEBUG
      log_d("Interpreter State found @ raddr: %p after %d iterations",
        self->is_raddr,
        INIT_RETRY_CNT - try_cnt
      );
      #endif

      return 0;
    }
  }

  // Educated guess failed. Try brute force now.
  void * upper_bound = self->map.heap.base + self->map.heap.size;

  for (
    register void ** raddr = (void **) self->map.heap.base;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (_py_proc__is_heap_raddr(self, raddr) && _py_proc__check_interp_state(self, raddr) == 0) {
      self->is_raddr = raddr;
      return 0;
    }
  }

  error = EPROCISTIMEOUT;
  return 1;
}


// ---- PUBLIC ----------------------------------------------------------------

// ----------------------------------------------------------------------------
py_proc_t *
py_proc_new() {
  py_proc_t * py_proc = (py_proc_t *) malloc(sizeof(py_proc_t));
  if (py_proc == NULL)
    error = EPROC;

  else {
    py_proc->pid      = 0;
    py_proc->bin_path = NULL;
    py_proc->is_raddr = NULL;

    py_proc->sym_loaded        = 0;
    py_proc->tstate_curr_raddr = NULL;
  }

  check_not_null(py_proc);
  return py_proc;
}


// ----------------------------------------------------------------------------
int
py_proc__attach(py_proc_t * self, pid_t pid) {
  self->pid = pid;

  if (_py_proc__wait_for_interp_state(self) == 0)
    return 0;

  log_error();
  return 1;
}


// ----------------------------------------------------------------------------
int
py_proc__start(py_proc_t * self, const char * exec, char * argv[]) {
  self->pid = fork();

  if (self->pid == 0) {
    execvp(exec, argv);
    log_e("Failed to fork process");
    exit(127);
  }
  else {
    if (_py_proc__wait_for_interp_state(self) == 0)
      return 0;
  }

  log_error();
  return 1;
}


// ----------------------------------------------------------------------------
int
py_proc__memcpy(py_proc_t * self, void * raddr, ssize_t size, void * dest) {
  return copy_memory(self->pid, raddr, size, dest) == size ? 0 : 1;
}


// ----------------------------------------------------------------------------
void
py_proc__wait(py_proc_t * self) {
  waitpid(self->pid, 0, 0);
}


// ----------------------------------------------------------------------------
void *
py_proc__get_istate_raddr(py_proc_t * self) {
  return self->is_raddr;
}


// ----------------------------------------------------------------------------
int
py_proc__is_running(py_proc_t * self) {
  kill(self->pid, 0);

  return errno == ESRCH ? 0 : 1;
}


// ----------------------------------------------------------------------------
void
py_proc__destroy(py_proc_t * self) {
  if (self == NULL)
    return;

  if (self->bin_path != NULL)
    free(self->bin_path);

  free(self);
}
