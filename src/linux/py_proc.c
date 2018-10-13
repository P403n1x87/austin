#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../dict.h"
#include "../py_proc.h"


#define BIN_MAP                  (1 << 0)
#define DYNSYM_MAP               (1 << 1)
#define RODATA_MAP               (1 << 2)
#define HEAP_MAP                 (1 << 3)
#define BSS_MAP                  (1 << 4)


#define _py_proc__get_elf_type(self, offset, dt) (py_proc__memcpy(self, self->map.elf.base + offset, sizeof(dt), &dt))
#define ELF_SH_OFF(ehdr, i)            (ehdr.e_shoff + i * ehdr.e_shentsize)

#define DYNSYM_COUNT                   2

union {
  Elf32_Ehdr v32;
  Elf64_Ehdr v64;
} ehdr_v;

static const char * _dynsym_array[DYNSYM_COUNT] = {
  "_PyThreadState_Current",
  "_PyRuntime"
};

static long _dynsym_hash_array[DYNSYM_COUNT] = {
  0
};


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
    Elf64_Shdr * p_dynsym = self->map.dynsym.base;
    if (p_dynsym->sh_offset == 0)
      return 1;

    Elf64_Shdr * p_strtabsh = (Elf64_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

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

  munmap(elf_map, elf_map_size);
  close(fd);

  return hit_cnt;
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

  Elf32_Shdr  * p_shstrtab = elf_map + ELF_SH_OFF(ehdr, ehdr.e_shstrndx);
  char        * sh_name_base = elf_map + p_shstrtab->sh_offset;

  for (Elf32_Off sh_off = ehdr.e_shoff; \
    map_flag != (DYNSYM_MAP | RODATA_MAP) && sh_off < elf_map_size; \
    sh_off += ehdr.e_shentsize \
  ) {
    p_shdr = (Elf32_Shdr *) (elf_map + sh_off);

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
    Elf32_Shdr * p_dynsym = self->map.dynsym.base;
    if (p_dynsym->sh_offset == 0)
      return 1;

    Elf32_Shdr * p_strtabsh = (Elf32_Shdr *) (elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));

    // Search for dynamic symbols
    for (Elf32_Off tab_off = p_dynsym->sh_offset; \
      hit_cnt < DYNSYM_COUNT && tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
      tab_off += p_dynsym->sh_entsize
    ) {
      Elf32_Sym * sym      = (Elf32_Sym *) (elf_map + tab_off);
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

  munmap(elf_map, elf_map_size);
  close(fd);

  return hit_cnt;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf(py_proc_t * self) {
  Elf64_Ehdr ehdr = ehdr_v.v64;

  // Check magic
  if (_py_proc__get_elf_type(self, 0, ehdr_v) || ehdr.e_shoff == 0 || ehdr.e_shnum < 2 \
    || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F' \
  ) return 0;

  // NOTE: This runs sub-optimally when searching for a single symbol
  // Pre-hash symbol names
  if (_dynsym_hash_array[0] == 0) {
    for (register int i = 0; i < DYNSYM_COUNT; i++)
      _dynsym_hash_array[i] = string_hash((char *) _dynsym_array[i]);
  }

  // Dispatch
  switch (ehdr.e_ident[EI_CLASS]) {
  case ELFCLASS32:
    return _py_proc__analyze_elf32(self);

  case ELFCLASS64:
    return _py_proc__analyze_elf64(self);

  default:
    return 0;
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

  #ifdef DEBUG
  log_d("Opening %s", file_name);
  #endif // DEBUG

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

          #ifdef DEBUG
          log_d("Python binary path: %s\n", self->bin_path);
          #endif // DEBUG
        }
      }
      else {
        if ((needle = strstr(line, "[heap]\n")) != NULL) {
          if ((maps_flag & HEAP_MAP) == 0) {
            get_bounds(line, a, b);
            self->map.heap.base = (void *) a;
            self->map.heap.size = b - a;

            maps_flag |= HEAP_MAP;

            #ifdef DEBUG
            log_d("HEAP bounds: %s", line);
            #endif
          }
        }
        else if ((maps_flag & BSS_MAP) == 0 && (line[strlen(line)-2] == ' ')) {
          get_bounds(line, a, b);
          self->map.bss.base = (void *) a;
          self->map.bss.size = b - a;

          maps_flag |= BSS_MAP;

          #ifdef DEBUG
          log_d("BSS bounds: %s", line);
          #endif
        }
      }
    }

    fclose(fp);
    if (line != NULL) {
      free(line);
    }
  }

  if (error & EPROC) log_error();

  self->maps_loaded = maps_flag == (HEAP_MAP | BSS_MAP) ? 1 : 0;

  return 1 - self->maps_loaded;
}


// ----------------------------------------------------------------------------
static int
_py_proc__analyze_bin(py_proc_t * self) {
  if (self == NULL)
    return 1;

  if (self->maps_loaded == 0) {
    _py_proc__parse_maps_file(self);

    if (self->maps_loaded == 0)
      return 1;
  }

  if (self->sym_loaded)
    return 0;
  self->sym_loaded = _py_proc__analyze_elf(self) ? 1 : 0;

  return 1 - self->sym_loaded;
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
_py_proc__is_bss_raddr(py_proc_t * self, void * raddr) {
  if (self == NULL || raddr == NULL || self->map.bss.base == NULL)
    return 0;

  return (raddr >= self->map.bss.base && raddr < self->map.bss.base + self->map.bss.size)
    ? 1
    : 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__deref_interp_state(py_proc_t * self) {
  PyThreadState   tstate_current;
  _PyRuntimeState py_runtime;

  if (self == NULL)
    return 1;

  if (self->sym_loaded == 0)
    _py_proc__analyze_bin(self);

  if (self->sym_loaded == 0)
    return 1;

  if (!self->version) {
    self->version = _py_proc__get_version(self);
    if (!self->version)
      return 1;

    set_version(self->version);
  }

  if (self->py_runtime == NULL && self->tstate_curr_raddr == NULL)
    return -2;

  // Python 3.7 exposes the _PyRuntime symbol. This can be used to find the
  // head interpreter state.
  if (self->py_runtime != NULL) {
    // NOTE: With Python 3.7, this check causes the de-reference to fail even
    //       in cases where it shouldn't.
    // if (
    //   _py_proc__is_bss_raddr(self, self->py_runtime) == 0 &&
    //   _py_proc__is_heap_raddr(self, self->py_runtime) == 0
    // ) return -1;

    if (py_proc__get_type(self, self->py_runtime, py_runtime) != 0)
      return 1;

    if (_py_proc__check_interp_state(self, py_runtime.interpreters.head))
      return 1;

    self->is_raddr = py_runtime.interpreters.head;

    return 0;
  }

  if (
    _py_proc__is_bss_raddr(self, self->tstate_curr_raddr) == 0 &&
    _py_proc__is_heap_raddr(self, self->tstate_curr_raddr) == 0
  ) return -1;

  if (py_proc__get_type(self, self->tstate_curr_raddr, tstate_current) != 0)
    return 1;

  // 3.6.5 -> 3.6.6: _PyThreadState_Current doesn't seem what one would expect
  //                 anymore, but _PyThreadState_Current.prev is.
  if (
    V_FIELD(void*, tstate_current, py_thread, o_thread_id) == 0 && \
    V_FIELD(void*, tstate_current, py_thread, o_prev)      != 0
  ) {
    self->tstate_curr_raddr = V_FIELD(void*, tstate_current, py_thread, o_prev);
    return 1;
  }

  if (_py_proc__check_interp_state(self, V_FIELD(void*, tstate_current, py_thread, o_interp)))
    return 1;

  self->is_raddr = V_FIELD(void*, tstate_current, py_thread, o_interp);

  return 0;
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_bss(py_proc_t * self) {
  if (py_proc__memcpy(self, self->map.bss.base, self->map.bss.size, self->bss))
    return 1;

  void * upper_bound = self->bss + self->map.bss.size;
  for (
    register void ** raddr = (void **) self->bss;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (_py_proc__is_heap_raddr(self, *raddr) && _py_proc__check_interp_state(self, *raddr) == 0) {
      self->is_raddr = *raddr;
      return 0;
    }
  }

  return 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__scan_heap(py_proc_t * self) {
  // NOTE: This seems to be required by Python 2.7 on i386 Linux.
  void * upper_bound = self->map.heap.base + self->map.heap.size;
  for (
    register void ** raddr = (void **) self->map.heap.base;
    (void *) raddr < upper_bound;
    raddr++
  ) {
    if (_py_proc__check_interp_state(self, raddr) == 0) {
      self->is_raddr = raddr;
      return 0;
    }
  }

  return 1;
}


// ----------------------------------------------------------------------------
static int
_py_proc__wait_for_interp_state(py_proc_t * self) {
  register int try_cnt = INIT_RETRY_CNT;

  while (--try_cnt) {
    usleep(INIT_RETRY_SLEEP);

    switch (_py_proc__deref_interp_state(self)) {
    case 0:
      #ifdef DEBUG
      log_d("Interpreter State de-referenced @ raddr: %p after %d iterations",
        self->is_raddr,
        INIT_RETRY_CNT - try_cnt
      );
      #endif

      return 0;

    case -1:
      #ifdef DEBUG
      log_d("Symbol address not within VM maps (shared object?)");
      #endif
      try_cnt = 1;
      break;

    case -2:
      log_w("Null symbol references. This is unexpected on Linux.");
      try_cnt = 1;
      break;
    }
  }

  log_d("Unable to de-reference global symbols. Scanning the bss section...");

  // Copy .bss section from remote location
  self->bss = malloc(self->map.bss.size);
  if (self->bss == NULL)
    return 1;

  try_cnt = INIT_RETRY_CNT;
  while (--try_cnt) {
    usleep(INIT_RETRY_SLEEP);

    if (_py_proc__scan_bss(self) == 0)
      return 0;
  }

  log_w("Bss scan unsuccessful. Scanning heap directly...");

  // TODO: Consider copying heap over and check for pointers
  try_cnt = INIT_RETRY_CNT;
  while (--try_cnt) {
    usleep(INIT_RETRY_SLEEP);

    if (_py_proc__scan_heap(self) == 0)
      return 0;
  }

  error = EPROCISTIMEOUT;
  return 1;
}
