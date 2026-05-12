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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../mem.h"
#include "../platform.h"
#include "../resources.h"
#include "common.h"
#include "futils.h"
#include "proc/exe.h"
#include "proc/maps.h"

#include "../argparse.h"
#include "../cache.h"
#include "../hints.h"
#include "../py_proc.h"
#include "../py_string.h"
#include "../version.h"

#define BIN_MAP    (1 << 0)
#define DYNSYM_MAP (1 << 1)
#define RODATA_MAP (1 << 2)
#define BSS_MAP    (1 << 3)

// Get the offset of the ith section header
#define ELF_SH_OFF(ehdr, i) /* as */ (ehdr->e_shoff + i * ehdr->e_shentsize)

union {
    Elf32_Ehdr v32;
    Elf64_Ehdr v64;
} ehdr_v;

// ----------------------------------------------------------------------------
static void*
wait_thread(void* py_proc) {
    waitpid(((py_proc_t*)py_proc)->pid, 0, 0);
    return NULL;
}

// ----------------------------------------------------------------------------
static ssize_t
_file_size(char* file) {
    struct stat statbuf;

    if (fail(stat(file, &statbuf))) {
        set_error(IO, "Cannot stat file");
        FAIL_INT;
    }

    return statbuf.st_size;
}

// GCOV_EXCL_START
#define _DEF_ANALYZE_ELF(BITS, ADDR_SENTINEL)                                                                           \
    static Elf##BITS##_Addr _get_base_##BITS(Elf##BITS##_Ehdr* ehdr, void* elf_map) {                                   \
        for (int i = 0; i < ehdr->e_phnum; ++i) {                                                                       \
            Elf##BITS##_Phdr* phdr = (Elf##BITS##_Phdr*)(elf_map + ehdr->e_phoff + i * ehdr->e_phentsize);              \
            if (phdr->p_type == PT_LOAD)                                                                                \
                return phdr->p_vaddr - phdr->p_vaddr % phdr->p_align;                                                   \
        }                                                                                                               \
        return ADDR_SENTINEL;                                                                                           \
    } /* _get_base_##BITS */                                                                                            \
                                                                                                                        \
    static int _py_proc__analyze_elf##BITS(py_proc_t* self, void* elf_map, void* elf_base, proc_vm_map_block_t* bss) {  \
        register int symbols = 0;                                                                                       \
                                                                                                                        \
        Elf##BITS##_Ehdr* ehdr = elf_map;                                                                               \
                                                                                                                        \
        Elf##BITS##_Xword sht_size     = ehdr->e_shnum * ehdr->e_shentsize;                                             \
        Elf##BITS##_Off   elf_map_size = ehdr->e_shoff + sht_size;                                                      \
        Elf##BITS##_Shdr* p_shdr;                                                                                       \
                                                                                                                        \
        Elf##BITS##_Shdr* p_shstrtab   = elf_map + ELF_SH_OFF(ehdr, ehdr->e_shstrndx);                                  \
        char*             sh_name_base = elf_map + p_shstrtab->sh_offset;                                               \
        Elf##BITS##_Shdr* p_dynsym     = NULL;                                                                          \
        Elf##BITS##_Addr  base         = _get_base_##BITS(ehdr, elf_map);                                               \
                                                                                                                        \
        void*  bss_base = NULL;                                                                                         \
        size_t bss_size = 0;                                                                                            \
                                                                                                                        \
        if (base != ADDR_SENTINEL) {                                                                                    \
            log_d("ELF base @ %p", base);                                                                               \
            for (Elf##BITS##_Off sh_off = ehdr->e_shoff; sh_off < elf_map_size; sh_off += ehdr->e_shentsize) {          \
                p_shdr = (Elf##BITS##_Shdr*)(elf_map + sh_off);                                                         \
                if (p_shdr->sh_type == SHT_DYNSYM && strcmp(sh_name_base + p_shdr->sh_name, ".dynsym") == 0) {          \
                    p_dynsym = p_shdr;                                                                                  \
                } else if (strcmp(sh_name_base + p_shdr->sh_name, ".bss") == 0) {                                       \
                    bss_base = elf_base + (p_shdr->sh_addr - base);                                                     \
                    bss_size = p_shdr->sh_size;                                                                         \
                } else if (strcmp(sh_name_base + p_shdr->sh_name, ".PyRuntime") == 0) {                                 \
                    self->map.runtime.base = elf_base + (p_shdr->sh_addr - base);                                       \
                    self->map.runtime.size = p_shdr->sh_size;                                                           \
                }                                                                                                       \
            }                                                                                                           \
            if (isvalid(p_dynsym) && p_dynsym->sh_offset != 0) {                                                        \
                Elf##BITS##_Shdr* p_strtabsh = (Elf##BITS##_Shdr*)(elf_map + ELF_SH_OFF(ehdr, p_dynsym->sh_link));      \
                for (Elf##BITS##_Off tab_off  = p_dynsym->sh_offset; tab_off < p_dynsym->sh_offset + p_dynsym->sh_size; \
                     tab_off                 += p_dynsym->sh_entsize) {                                                 \
                    Elf##BITS##_Sym* sym      = (Elf##BITS##_Sym*)(elf_map + tab_off);                                  \
                    char*            sym_name = (char*)(elf_map + p_strtabsh->sh_offset + sym->st_name);                \
                    void*            value    = elf_base + (sym->st_value - base);                                      \
                    if ((symbols += _py_proc__check_sym(self, sym_name, value)) >= DYNSYM_COUNT)                        \
                        break;                                                                                          \
                }                                                                                                       \
            }                                                                                                           \
        }                                                                                                               \
        if (symbols < DYNSYM_MANDATORY) {                                                                               \
            set_error(BINARY, "Not all required symbols found");                                                        \
            FAIL;                                                                                                       \
        }                                                                                                               \
        bss->base = bss_base;                                                                                           \
        bss->size = bss_size;                                                                                           \
        log_d("BSS @ %p (size %x, offset %x)", bss_base, bss_size, bss_base - elf_base);                                \
        SUCCESS;                                                                                                        \
    } /* _py_proc__analyze_elf##BITS */

_DEF_ANALYZE_ELF(64, UINT64_MAX)
_DEF_ANALYZE_ELF(32, UINT32_MAX)
#undef _DEF_ANALYZE_ELF
// GCOV_EXCL_STOP

// ----------------------------------------------------------------------------
static int
_elf_check(Elf64_Ehdr* ehdr) {
    return (ehdr->e_shoff == 0 || ehdr->e_shnum < 2 || memcmp(ehdr->e_ident, ELFMAG, SELFMAG));
}

// ----------------------------------------------------------------------------
static int
_py_proc__analyze_elf(py_proc_t* self, char* path, void* elf_base, proc_vm_map_block_t* bss) {
    cu_fd fd = open(path, O_RDONLY);
    if (fd == -1) {
        set_error(IO, "Cannot open binary file");
        FAIL;
    }

    cu_map_t*   binary_map  = NULL;
    size_t      binary_size = 0;
    struct stat s;

    if (fstat(fd, &s) == -1) { // GCOV_EXCL_START
        set_error(IO, "Cannot determine size of binary file");
        FAIL;
    } // GCOV_EXCL_STOP

    binary_size = s.st_size;

    binary_map = map_new(fd, binary_size, MAP_PRIVATE);
    if (!isvalid(binary_map)) { // GCOV_EXCL_START
        set_error(IO, "Cannot map binary file to memory");
        FAIL;
    } // GCOV_EXCL_STOP

    Elf64_Ehdr* ehdr = binary_map->addr;
    log_t("Analysing ELF");

    if (fail(_elf_check(ehdr))) { // GCOV_EXCL_START
        set_error(BINARY, "Bad ELF header");
        FAIL;
    } // GCOV_EXCL_STOP

    // Dispatch
    switch (ehdr->e_ident[EI_CLASS]) {
    case ELFCLASS64:
        log_d("%s is 64-bit ELF", path);
        return _py_proc__analyze_elf64(self, binary_map->addr, elf_base, bss);

    case ELFCLASS32: // GCOV_EXCL_START
        log_d("%s is 32-bit ELF", path);
        return _py_proc__analyze_elf32(self, binary_map->addr, elf_base, bss);

    default:
        set_error(BINARY, "Invalid ELF class");
        FAIL;
    } // GCOV_EXCL_STOP
} /* _py_proc__analyze_elf */

// Read the dynamic symbol table directly from the target process's virtual
// address space.  Parses ELF header → program headers → PT_DYNAMIC →
// DT_SYMTAB / DT_STRTAB / DT_HASH.  Section headers are not needed —
// they exist only in the file.  BSS is zeroed; callers use the proc_maps
// heuristic as fallback.
// GCOV_EXCL_START
#define _DEF_ANALYZE_ELF_FROM_MEMORY(BITS)                                                                             \
    static int _py_proc__analyze_elf##BITS##_from_memory(py_proc_t* self, void* elf_base, proc_vm_map_block_t* bss) {  \
        bss->base = NULL;                                                                                              \
        bss->size = 0;                                                                                                 \
                                                                                                                       \
        Elf##BITS##_Ehdr ehdr;                                                                                         \
        if (fail(copy_memory(self->ref, elf_base, sizeof(Elf##BITS##_Ehdr), &ehdr)))                                   \
            FAIL;                                                                                                      \
        if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS##BITS) {                  \
            set_error(BINARY, "Bad ELF header in process memory");                                                     \
            FAIL;                                                                                                      \
        }                                                                                                              \
        if (ehdr.e_phnum == 0 || ehdr.e_phentsize < sizeof(Elf##BITS##_Phdr)) {                                        \
            set_error(BINARY, "No usable program headers");                                                            \
            FAIL;                                                                                                      \
        }                                                                                                              \
        size_t            phdrs_size = (size_t)ehdr.e_phnum * ehdr.e_phentsize;                                        \
        Elf##BITS##_Phdr* phdrs      = malloc(phdrs_size);                                                             \
        if (!isvalid(phdrs)) {                                                                                         \
            set_error(MALLOC, "Cannot allocate program headers buffer");                                               \
            FAIL;                                                                                                      \
        }                                                                                                              \
        if (fail(copy_memory(self->ref, (void*)((uintptr_t)elf_base + (uintptr_t)ehdr.e_phoff), phdrs_size, phdrs))) { \
            free(phdrs);                                                                                               \
            FAIL;                                                                                                      \
        }                                                                                                              \
        /* First pass: ELF file base from first PT_LOAD */                                                             \
        Elf##BITS##_Addr file_base = (Elf##BITS##_Addr)(-1);                                                           \
        for (int i = 0; i < ehdr.e_phnum; i++) {                                                                       \
            if (phdrs[i].p_type == PT_LOAD) {                                                                          \
                file_base = phdrs[i].p_vaddr - phdrs[i].p_vaddr % phdrs[i].p_align;                                    \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
        if (file_base == (Elf##BITS##_Addr)(-1)) {                                                                     \
            free(phdrs);                                                                                               \
            set_error(BINARY, "No PT_LOAD segment");                                                                   \
            FAIL;                                                                                                      \
        }                                                                                                              \
        /* Second pass: locate PT_DYNAMIC */                                                                           \
        void*  dyn_raddr = NULL;                                                                                       \
        size_t dyn_size  = 0;                                                                                          \
        for (int i = 0; i < ehdr.e_phnum; i++) {                                                                       \
            if (phdrs[i].p_type == PT_DYNAMIC) {                                                                       \
                dyn_raddr = (void*)((uintptr_t)elf_base + (uintptr_t)phdrs[i].p_vaddr - (uintptr_t)file_base);         \
                dyn_size  = phdrs[i].p_memsz;                                                                          \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
        free(phdrs);                                                                                                   \
        if (!isvalid(dyn_raddr)) {                                                                                     \
            set_error(BINARY, "No PT_DYNAMIC segment");                                                                \
            FAIL;                                                                                                      \
        }                                                                                                              \
        size_t           ndyn = dyn_size / sizeof(Elf##BITS##_Dyn);                                                    \
        Elf##BITS##_Dyn* dyn  = malloc(dyn_size);                                                                      \
        if (!isvalid(dyn)) {                                                                                           \
            set_error(MALLOC, "Cannot allocate dynamic section buffer");                                               \
            FAIL;                                                                                                      \
        }                                                                                                              \
        if (fail(copy_memory(self->ref, dyn_raddr, dyn_size, dyn))) {                                                  \
            free(dyn);                                                                                                 \
            FAIL;                                                                                                      \
        }                                                                                                              \
        void*    symtab_raddr = NULL;                                                                                  \
        void*    strtab_raddr = NULL;                                                                                  \
        void*    hash_raddr   = NULL;                                                                                  \
        uint32_t strtab_size  = 0;                                                                                     \
        for (size_t i = 0; i < ndyn; i++) {                                                                            \
            switch (dyn[i].d_tag) {                                                                                    \
            case DT_SYMTAB:                                                                                            \
                symtab_raddr = (void*)(uintptr_t)dyn[i].d_un.d_ptr;                                                    \
                break;                                                                                                 \
            case DT_STRTAB:                                                                                            \
                strtab_raddr = (void*)(uintptr_t)dyn[i].d_un.d_ptr;                                                    \
                break;                                                                                                 \
            case DT_STRSZ:                                                                                             \
                strtab_size = (uint32_t)dyn[i].d_un.d_val;                                                             \
                break;                                                                                                 \
            case DT_HASH:                                                                                              \
                hash_raddr = (void*)(uintptr_t)dyn[i].d_un.d_ptr;                                                      \
                break;                                                                                                 \
            case DT_NULL:                                                                                              \
                i = ndyn;                                                                                              \
                break;                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
        free(dyn);                                                                                                     \
        if (!isvalid(symtab_raddr) || !isvalid(strtab_raddr)) {                                                        \
            set_error(BINARY, "Dynamic symbol/string table not found");                                                \
            FAIL;                                                                                                      \
        }                                                                                                              \
        /* Symbol count from DT_HASH nchain; cap at 8192 if absent */                                                  \
        uint32_t nsyms       = 0;                                                                                      \
        bool     nsyms_exact = false;                                                                                  \
        if (isvalid(hash_raddr)) {                                                                                     \
            uint32_t hdr[2];                                                                                           \
            if (success(copy_memory(self->ref, hash_raddr, sizeof(hdr), hdr)) && hdr[1] > 0) {                         \
                nsyms       = hdr[1];                                                                                  \
                nsyms_exact = true;                                                                                    \
            }                                                                                                          \
        }                                                                                                              \
        if (nsyms == 0)                                                                                                \
            nsyms = 8192;                                                                                              \
        if (strtab_size == 0 || strtab_size > (1 << 20))                                                               \
            strtab_size = 65536;                                                                                       \
        cu_char* strtab = malloc(strtab_size + 1);                                                                     \
        if (!isvalid(strtab)) {                                                                                        \
            set_error(MALLOC, "Cannot allocate string table buffer");                                                  \
            FAIL;                                                                                                      \
        }                                                                                                              \
        strtab[strtab_size] = '\0';                                                                                    \
        if (fail(copy_memory(self->ref, strtab_raddr, strtab_size, strtab)))                                           \
            FAIL;                                                                                                      \
        register int symbols = 0;                                                                                      \
        if (nsyms_exact) {                                                                                             \
            /* Exact count known: batch-read the whole symtab in one syscall */                                        \
            size_t   symtab_size = (size_t)nsyms * sizeof(Elf##BITS##_Sym);                                            \
            cu_void* symtab      = malloc(symtab_size);                                                                \
            if (isvalid(symtab) && success(copy_memory(self->ref, symtab_raddr, symtab_size, symtab))) {               \
                for (uint32_t i = 0; i < nsyms && symbols < DYNSYM_COUNT; i++) {                                       \
                    Elf##BITS##_Sym* sym = &((Elf##BITS##_Sym*)symtab)[i];                                             \
                    if (sym->st_value == 0 || sym->st_name >= strtab_size)                                             \
                        continue;                                                                                      \
                    void* value  = (void*)((uintptr_t)elf_base + (uintptr_t)sym->st_value - (uintptr_t)file_base);     \
                    symbols     += _py_proc__check_sym(self, strtab + sym->st_name, value);                            \
                }                                                                                                      \
            }                                                                                                          \
        } else {                                                                                                       \
            /* Count unknown: read one page at a time to amortise syscall overhead */                                  \
            size_t   page_size = (size_t)getpagesize();                                                                \
            uint32_t chunk     = (uint32_t)(page_size / sizeof(Elf##BITS##_Sym));                                      \
            cu_void* chunk_buf = malloc(page_size);                                                                    \
            if (!isvalid(chunk_buf)) {                                                                                 \
                set_error(MALLOC, "Cannot allocate symbol chunk buffer");                                              \
                FAIL;                                                                                                  \
            }                                                                                                          \
            for (uint32_t i = 0; i < nsyms && symbols < DYNSYM_COUNT; i += chunk) {                                    \
                uint32_t n  = (i + chunk <= nsyms) ? chunk : (nsyms - i);                                              \
                size_t   sz = n * sizeof(Elf##BITS##_Sym);                                                             \
                if (fail(copy_memory(                                                                                  \
                        self->ref, (void*)((uintptr_t)symtab_raddr + i * sizeof(Elf##BITS##_Sym)), sz, chunk_buf       \
                    ))) {                                                                                              \
                    /* Chunk read failed: fall back to single-symbol reads to recover valid data */                    \
                    for (uint32_t k = 0; k < n && symbols < DYNSYM_COUNT; k++) {                                       \
                        Elf##BITS##_Sym sym;                                                                           \
                        if (fail(copy_memory(                                                                          \
                                self->ref, (void*)((uintptr_t)symtab_raddr + (i + k) * sizeof(Elf##BITS##_Sym)),       \
                                sizeof(Elf##BITS##_Sym), &sym                                                          \
                            )))                                                                                        \
                            break;                                                                                     \
                        if (sym.st_value == 0 || sym.st_name >= strtab_size)                                           \
                            continue;                                                                                  \
                        void* value  = (void*)((uintptr_t)elf_base + (uintptr_t)sym.st_value - (uintptr_t)file_base);  \
                        symbols     += _py_proc__check_sym(self, strtab + sym.st_name, value);                         \
                    }                                                                                                  \
                    break;                                                                                             \
                }                                                                                                      \
                for (uint32_t j = 0; j < n && symbols < DYNSYM_COUNT; j++) {                                           \
                    Elf##BITS##_Sym* sym = &((Elf##BITS##_Sym*)chunk_buf)[j];                                          \
                    if (sym->st_value == 0 || sym->st_name >= strtab_size)                                             \
                        continue;                                                                                      \
                    void* value  = (void*)((uintptr_t)elf_base + (uintptr_t)sym->st_value - (uintptr_t)file_base);     \
                    symbols     += _py_proc__check_sym(self, strtab + sym->st_name, value);                            \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
        if (symbols < DYNSYM_MANDATORY) {                                                                              \
            set_error(BINARY, "Not all required symbols found in process memory");                                     \
            FAIL;                                                                                                      \
        }                                                                                                              \
        SUCCESS;                                                                                                       \
    } /* _py_proc__analyze_elf##BITS##_from_memory */

_DEF_ANALYZE_ELF_FROM_MEMORY(64)
_DEF_ANALYZE_ELF_FROM_MEMORY(32)
#undef _DEF_ANALYZE_ELF_FROM_MEMORY

// ----------------------------------------------------------------------------
// Dispatcher: read ELF class from process memory and call the right variant.
static int
_py_proc__analyze_elf_from_memory(py_proc_t* self, void* elf_base, proc_vm_map_block_t* bss) {
    unsigned char ei_class;
    if (fail(copy_memory(self->ref, (void*)((uintptr_t)elf_base + EI_CLASS), 1, &ei_class)))
        FAIL;
    switch (ei_class) {
    case ELFCLASS64:
        return _py_proc__analyze_elf64_from_memory(self, elf_base, bss);
    case ELFCLASS32:
        return _py_proc__analyze_elf32_from_memory(self, elf_base, bss);
    default:
        set_error(BINARY, "Unknown ELF class in process memory");
        FAIL;
    }
} /* _py_proc__analyze_elf_from_memory */

// ----------------------------------------------------------------------------
static int
_py_proc__inspect_vm_maps(py_proc_t* self) {
    int                 maps_flag = 0;
    struct vm_map*      map       = NULL;
    proc_vm_map_block_t bss;

    cu_proc_map_t* proc_maps = proc_map_new(self->pid);
    if (!isvalid(proc_maps)) {
        FAIL;
    }

    sfree(self->bin_path);
    sfree(self->lib_path);

    self->map.exe.base = NULL;
    self->map.exe.size = 0;

    cu_void* pd_mem = calloc(1, sizeof(struct proc_desc));
    if (!isvalid(pd_mem)) { // GCOV_EXCL_START
        set_error(MALLOC, "Cannot allocate memory for proc_desc");
        FAIL;
    } // GCOV_EXCL_STOP
    struct proc_desc* pd = pd_mem;

    proc_map_t* first_binary_map = NULL;

    if (fail(proc_exe_readlink(self->pid, pd->exe_path, sizeof(pd->exe_path)))) {
        // We cannot readlink the executable path so we take the first memory map
        PROC_MAP_ITER(proc_maps, m) {
            if (isvalid(m->pathname)) {
                strncpy(pd->exe_path, m->pathname, sizeof(pd->exe_path) - 1);
                first_binary_map = m;
                break;
            }
        }
        if (!isvalid(first_binary_map)) { // GCOV_EXCL_START
            set_error(OS, "Failed to infer the executable path");
            FAIL;
        } // GCOV_EXCL_STOP
    } else {
        first_binary_map = proc_map__first(proc_maps, pd->exe_path);
        if (!isvalid(first_binary_map))
            FAIL;
    }

    log_d("Executable path: %s", pd->exe_path);

    map       = &(pd->maps[MAP_BIN]);
    map->base = first_binary_map->address;
    map->size = first_binary_map->size;
    map->path = proc_map_file_path(self->pid, pd->exe_path, map->base, map->size);
    if (!isvalid(map->path)) {
        FAIL; // GCOV_EXCL_LINE
    }
    map->file_size = _file_size(map->path);
    log_d("Analyzing binary map from memory: %s @ %p", pd->exe_path, map->base);
    map->has_symbols = success(_py_proc__analyze_elf_from_memory(self, map->base, &bss));
    if (!map->has_symbols)
        map->has_symbols = success(_py_proc__analyze_elf(self, map->path, map->base, &bss));
    if (map->has_symbols) {
        map->bss_base = bss.base;
        map->bss_size = bss.size;
    }
    log_d("Binary path: %s (symbols: %d)", map->path, map->has_symbols);

    size_t page_size = getpagesize();

    if (map->bss_size == 0) {
        // Find the BSS section for the binary
        PROC_MAP_ITER(first_binary_map, m) {
            if (!isvalid(m->pathname) && m->perms == (PERMS_READ | PERMS_WRITE) && m->size > 0) {
                map           = &(pd->maps[MAP_BIN]);
                map->bss_base = m->address - page_size;
                map->bss_size = m->size + page_size;
                log_d("BSS section found @ %p (size %x)", self->map.bss.base, self->map.bss.size);
                break;
            }
        }
    }

    if (!map->has_symbols) {
        // Find the runtime section for the binary
        PROC_MAP_ITER(first_binary_map, m) {
            if (m->perms == (PERMS_READ | PERMS_WRITE) && isvalid(m->pathname)
                && strcmp(m->pathname, pd->exe_path) == 0) {
                self->map.runtime.base = m->address - page_size;
                self->map.runtime.size = m->size + page_size;
                log_d("PyRuntime section found @ %p (size %x)", self->map.runtime.base, self->map.runtime.size);
                break;
            }
        }
    }

    proc_map_t* first_lib_map = proc_map__first_submatch(proc_maps, LIB_NEEDLE);
    if (isvalid(first_lib_map)) {
        char* lib_path
            = proc_map_file_path(self->pid, first_lib_map->pathname, first_lib_map->address, first_lib_map->size);
        log_d("Analyzing library map from memory: %s @ %p", first_lib_map->pathname, first_lib_map->address);
        bool lib_has_symbols
            = success(_py_proc__analyze_elf_from_memory(self, first_lib_map->address, &bss))
           || (isvalid(lib_path) && success(_py_proc__analyze_elf(self, lib_path, first_lib_map->address, &bss)));
        if (lib_has_symbols) {
            // The library binary has symbols
            map = &(pd->maps[MAP_LIBSYM]);

            map->path        = lib_path;
            map->file_size   = isvalid(lib_path) ? _file_size(lib_path) : 0;
            map->base        = first_lib_map->address;
            map->size        = first_lib_map->size;
            map->has_symbols = true;
            map->bss_base    = bss.base;
            map->bss_size    = bss.size;

            log_d("Library path: %s (with symbols)", lib_path ? lib_path : "(from memory)");
        } else {
            sfree(lib_path);
            // We look for something matching "libpythonX.Y"
            PROC_MAP_ITER(first_lib_map, m) {
                unsigned int v;
                char*        needle = strstr(m->pathname, LIB_NEEDLE);
                if (sscanf(needle, "libpython%u.%u", &v, &v) == 2) {
                    map = &(pd->maps[MAP_LIBNEEDLE]);

                    map->path = proc_map_file_path(self->pid, m->pathname, m->address, m->size);
                    if (!isvalid(map->path))
                        FAIL; // GCOV_EXCL_LINE

                    map->file_size   = _file_size(map->path);
                    map->base        = m->address;
                    map->size        = m->size;
                    map->has_symbols = false;

                    log_d("Library path: %s (from pattern match)", map->path);

                    break;
                }
            }
        }
    }

    // If library analysis found symbols but no BSS (e.g. via memory analysis),
    // scan proc_maps for an anonymous RW region after the library — same
    // heuristic used for the binary above.
    if (isvalid(first_lib_map) && isvalid(pd->maps[MAP_LIBSYM].path) && pd->maps[MAP_LIBSYM].bss_size == 0) {
        PROC_MAP_ITER(first_lib_map, m) {
            if (!isvalid(m->pathname) && m->perms == (PERMS_READ | PERMS_WRITE) && m->size > 0) {
                pd->maps[MAP_LIBSYM].bss_base = m->address - page_size;
                pd->maps[MAP_LIBSYM].bss_size = m->size + page_size;
                log_d(
                    "Library BSS section found @ %p (size %zx)", pd->maps[MAP_LIBSYM].bss_base,
                    pd->maps[MAP_LIBSYM].bss_size
                );
                break;
            }
        }
    }

    // If the library map is not valid, use the needle map
    if (!isvalid(pd->maps[MAP_LIBSYM].path)) {
        pd->maps[MAP_LIBSYM]         = pd->maps[MAP_LIBNEEDLE];
        pd->maps[MAP_LIBNEEDLE].path = NULL;
    }

    // Work out paths
    self->bin_path = pd->maps[MAP_BIN].path;
    self->lib_path = pd->maps[MAP_LIBSYM].path;

    // Work out binary map
    for (int i = 0; i < MAP_COUNT; i++) {
        map = &(pd->maps[i]);
        if (map->has_symbols) {
            self->map.exe.base  = map->base;
            self->map.exe.size  = map->size;
            maps_flag          |= BIN_MAP;
            self->sym_loaded    = true;
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

    // Work out BSS map: prefer library BSS (from section headers) if present.
    // Memory-based analysis leaves BSS NULL since section headers are not
    // mapped; fall back to the binary BSS which is populated by the proc_maps
    // heuristic.
    int map_index      = isvalid(pd->maps[MAP_LIBSYM].path) ? MAP_LIBSYM : MAP_BIN;
    self->map.bss.base = pd->maps[map_index].bss_base;
    self->map.bss.size = pd->maps[map_index].bss_size;
    if (!isvalid(self->map.bss.base) && map_index == MAP_LIBSYM) {
        self->map.bss.base = pd->maps[MAP_BIN].bss_base;
        self->map.bss.size = pd->maps[MAP_BIN].bss_size;
    }
    if (!isvalid(self->map.bss.base)) {
        set_error(OS, "Failed to find valid BSS map");
        FAIL;
    }

    if (!(maps_flag & (BIN_MAP))) {
        set_error(OS, "No usable Python binary found");
        FAIL;
    }

    log_d("BSS map %d from %s @ %p", map_index, pd->maps[map_index].path, self->map.bss.base);
    log_d("VM maps parsing result: bin=%s lib=%s flags=%d", self->bin_path, self->lib_path, maps_flag);

    SUCCESS;
} /* _py_proc__inspect_vm_maps */

// ----------------------------------------------------------------------------
static ssize_t
_py_proc__get_resident_memory(py_proc_t* self) {
    cu_FILE* statm = fopen(self->extra->statm_file, "rb");
    if (statm == NULL) { // GCOV_EXCL_START
        set_error(IO, "Cannot open statm file");
        FAIL_INT;
    } // GCOV_EXCL_STOP

    ssize_t size, resident;
    if (fscanf(statm, "%zd %zd", &size, &resident) != 2) { // GCOV_EXCL_START
        set_error(OS, "Failed to parse statm file");
        FAIL_INT; // cppcheck-suppress [resourceLeak]
    } // GCOV_EXCL_STOP

    return resident * self->extra->page_size; // cppcheck-suppress [resourceLeak]
} /* _py_proc__get_resident_memory */

// ----------------------------------------------------------------------------
#define RANGES_MAX 256

char        pathname[1024];
char        prevpathname[1024];
vm_range_t* ranges[RANGES_MAX];

static int
_py_proc__get_vm_maps(py_proc_t* self) {
    vm_range_tree_t* tree = NULL;
    cu_proc_map_t*   maps = NULL;

    // Build the range tree when native mode is active; it is needed to resolve
    // PC → binary path + load base at runtime.  pargs.where is covered by
    // pargs_native: on austinp native mode is always on, and on plain austin
    // --where without -n produces no native frames to resolve.
    //
    // The tree is rebuilt on every call because the target may have dlopen'd
    // new libraries since the last time we scanned the maps. The old tree is
    // destroyed first to avoid leaking memory.
    if (pargs_native) {
        // Recreate dance.
        tree = vm_range_tree_new();
        vm_range_tree__destroy(self->maps_tree);
        self->maps_tree = tree;
    }

    maps = proc_map_new(self->pid);
    if (!isvalid(maps))
        FAIL; // GCOV_EXCL_LINE

    log_d("Rebuilding vm ranges tree");

    int nrange = 0;
    PROC_MAP_ITER(maps, m) {
        if (nrange >= RANGES_MAX) { // GCOV_EXCL_START
            log_e("Too many ranges");
            break;
        } // GCOV_EXCL_STOP

        if (!isvalid(m->pathname))
            continue;

        if (pargs_native) {
            if (strcmp(m->pathname, prevpathname)) {
                ranges[nrange++]
                    = vm_range_new((addr_t)m->address, ((addr_t)m->address) + m->size, strdup(m->pathname));
                strcpy(prevpathname, m->pathname);
            } else
                ranges[nrange - 1]->hi = ((addr_t)m->address) + m->size;
        } else {
            // We print the maps instead so that we can resolve them later and use
            // the CPU more efficiently to collect samples.
            event_handler__emit_metadata(
                "map", "%zx-%zx %s", (addr_t)m->address, ((addr_t)m->address) + m->size, m->pathname
            );
        }
    }

    for (int i = 0; i < nrange; i++)
        vm_range_tree__add(tree, (vm_range_t*)ranges[i]);

    SUCCESS;
} /* _py_proc__get_vm_maps */

// ----------------------------------------------------------------------------
static int
_py_proc__init(py_proc_t* self) {
    if (!isvalid(self)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid process structure");
        FAIL;
    } // GCOV_EXCL_STOP

    if (fail(_py_proc__inspect_vm_maps(self))) {
        FAIL;
    }

    // We try to copy some remote memory to check that we have the permissions
    // to do so.
    char    c;
    raddr_t addr = self->symbols[DYNSYM_RUNTIME];
    if (!isvalid(addr))
        addr = self->map.bss.base;
    if (!isvalid(addr) || fail(copy_memory(self->ref, self->symbols[DYNSYM_RUNTIME], sizeof(c), &c)))
        FAIL;

    self->extra->page_size = get_page_size();
    log_d("Page size: %u", self->extra->page_size);

    sprintf(self->extra->statm_file, "/proc/%d/statm", self->pid);

    self->last_resident_memory = _py_proc__get_resident_memory(self);

    if (pargs_native)
        _py_proc__get_vm_maps(self);

    SUCCESS;
} /* _py_proc__init */

// ----------------------------------------------------------------------------
// Liveness check with zombie and PID-reuse detection.  kill(pid, 0) alone
// returns success for zombies and for any process that happens to reuse the
// PID after the target has been reaped; both cases would keep the py_proc__init
// retry loop spinning for its full timeout on target exit.  /proc/<pid>/stat
// yields both the state code (to reject Zombie/Dead) and starttime (to detect
// PID reuse across reaping).  starttime is captured on the first successful
// call and compared on subsequent ones.
static inline bool
_py_proc__is_running(py_proc_t* self) {
    char               state;
    unsigned long long starttime;
    if (!proc_stat_read(self->pid, &state, &starttime))
        return false;

    if (state == 'Z' || state == 'X')
        return false;

    if (self->extra->starttime == 0)
        self->extra->starttime = starttime;
    else if (self->extra->starttime != starttime)
        return false; // PID has been reused by a different process

    return true;
}

// ----------------------------------------------------------------------------
pid_t
_get_nspid(pid_t pid) {
    cu_char* line  = NULL;
    size_t   len   = 0;
    pid_t    nspid = 0;
    pid_t this     = 0;

    cu_FILE* status = _procfs(pid, "status");
    if (!isvalid(status)) { // GCOV_EXCL_START
        log_e("Cannot get namespace PID for %d", pid);
        return 0;
    } // GCOV_EXCL_STOP

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
_infer_tid_field_offset(py_thread_t* py_thread) {
    py_proc_t*       proc  = py_thread->proc;
    proc_extra_info* extra = proc->extra;

    if (extra->pthread_tid_offset != 0) {
        // We already have the offset so nothing more to do
        SUCCESS;
    }

    if (fail(read_pthread_t(py_thread->proc, (void*)py_thread->tid))) { // GCOV_EXCL_START
        FAIL;
    } // GCOV_EXCL_STOP

    log_d("pthread_t at %p", py_thread->tid);

    // If the target process is in a different PID namespace, we need to get its
    // other PID to be able to determine the offset of the TID field.
    proc_ref_t pref  = proc->ref;
    pid_t      nspid = _get_nspid(pref);

    for (register int i = 0; i < PTHREAD_BUFFER_ITEMS; i++) {
        if (pref == extra->_pthread_buffer[i] || (nspid && nspid == extra->_pthread_buffer[i])) {
            log_d("TID field offset: %d", i);
            extra->pthread_tid_offset = i;
            SUCCESS;
        }
    }

    // Fall-back to smaller steps if we failed
    for (register int i = 0; i < PTHREAD_BUFFER_ITEMS * (sizeof(uintptr_t) / sizeof(pid_t)); i++) {
        if (pref == (pid_t)((pid_t*)extra->_pthread_buffer)[i]
            || (nspid && nspid == (pid_t)((pid_t*)extra->_pthread_buffer)[i])) { // GCOV_EXCL_START
            log_d("TID field offset (from fall-back): %d", i);
            extra->pthread_tid_offset = -i;
            SUCCESS;
        }
    }

    extra->pthread_tid_offset = 0;

    set_error(OS, "Failed to find TID field offset");
    FAIL;
} // GCOV_EXCL_STOP

#endif
