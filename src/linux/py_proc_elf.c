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
