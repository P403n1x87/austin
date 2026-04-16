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

#pragma once

#ifdef HAVE_BFD

// ---- austinp: BFD-based symbol resolution -----------------------------------
// This source has been adapted from
// https://github.com/bminor/binutils-gdb/blob/ce230579c65b9e04c830f35cb78ff33206e65db1/binutils/addr2line.c

#define PACKAGE "austinp" // https://github.com/P403n1x87/austin/issues/152

#include <bfd.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_LIBERTY
#include <libiberty/demangle.h>
#endif

#include "../hints.h"
#include "../logging.h"
#include "../py_string.h"
#include "../stack.h"

static asymbol** syms; /* Symbol table.  */

static void
slurp_symtab(bfd*);
static void
find_address_in_section(bfd*, asection*, void*);

#define string__startswith(str, head) (strncmp(head, str, strlen(head)) == 0)

static bfd_vma      pc;
static const char*  filename;
static const char*  functionname;
static unsigned int line;
static unsigned int discriminator;

static void
slurp_symtab(bfd* abfd) {
    long storage;
    long symcount;
    bool dynamic = false;

    if (!(bfd_get_file_flags(abfd) & HAS_SYMS))
        return;

    storage = bfd_get_symtab_upper_bound(abfd);
    if (storage == 0) {
        storage = bfd_get_dynamic_symtab_upper_bound(abfd);
        dynamic = true;
    }
    if (storage < 0)
        return;

    syms = (asymbol**)malloc(storage);
    if (dynamic)
        symcount = bfd_canonicalize_dynamic_symtab(abfd, syms);
    else
        symcount = bfd_canonicalize_symtab(abfd, syms);

    if (symcount < 0) {
        free(syms);
        syms = NULL;
    }
}

static void
find_address_in_section(bfd* abfd, asection* section, void* data ATTRIBUTE_UNUSED) {
    bfd_vma       vma;
    bfd_size_type size;

    if (filename != NULL)
        return;

    if (!(bfd_section_flags(section) & SEC_ALLOC))
        return;

    vma = bfd_section_vma(section);
    if (pc < vma)
        return;

    size = bfd_section_size(section);
    if (pc >= vma + size)
        return;

    bfd_find_nearest_line_discriminator(abfd, section, syms, pc - vma, &filename, &functionname, &line, &discriminator);
}

static inline frame_t*
get_native_frame(const char* file_name, bfd_vma addr, key_dt frame_key) {
    bfd*   abfd;
    char** matching;

    abfd = bfd_openr(file_name, NULL);
    if (abfd == NULL) { // GCOV_EXCL_START
        log_e("Failed to open %s", file_name);
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    abfd->flags |= BFD_DECOMPRESS;

    if (bfd_check_format(abfd, bfd_archive)) { // GCOV_EXCL_START
        log_e("BFD format check failed");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    if (!bfd_check_format_matches(abfd, bfd_object, &matching)) { // GCOV_EXCL_START
        free(matching);
        log_d("BFD format matches check failed.");
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    slurp_symtab(abfd);

    filename = functionname = NULL;
    line = discriminator = 0;
    pc                   = addr;

    bfd_map_over_sections(abfd, find_address_in_section, NULL);

    const char* name = functionname;
    if (name == NULL || *name == '\0')
        name = "<unnamed>";
#ifdef HAVE_LIBERTY
    else {
        char* alloc = bfd_demangle(abfd, name, DMGL_PARAMS | DMGL_ANSI);
        if (alloc != NULL)
            name = alloc;
    }
#endif

    free(syms);
    syms = NULL;

    frame_t* frame = isvalid(filename) && isvalid(name) ? frame_new(
                                                              frame_key, cached_string_new(0, (char*)filename),
                                                              cached_string_new(0, (char*)name), line, 0, 0, 0
                                                          )
                                                        : NULL;

    bfd_close(abfd);

    return frame;
}

#endif /* HAVE_BFD */

#if defined(AUSTINP) || defined(__x86_64__) || defined(__aarch64__)

// ELF native symbol resolution for Linux (x86-64 / aarch64).
// Used as the primary resolver for austin -n and as a fallback for austinp
// when unw_get_proc_name cannot name a frame.
//
// Builds a sorted symbol table from the on-disk ELF .symtab (preferred) or
// .dynsym, applies the ASLR slide computed from the first executable PT_LOAD
// segment, and performs a binary search to resolve a PC to a function name.
// Results are cached per binary path so file I/O happens at most once per
// unique loaded binary.

#include <elf.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "../cache.h"
#include "../logging.h"
#include "../py_string.h"
#include "../resources.h"

// Select ELF word-size types based on the target ABI so the same code
// handles both 64-bit (aarch64, ppc64le, x86-64) and 32-bit (armv7) targets.
#ifdef __LP64__
typedef Elf64_Ehdr _Elf_Ehdr;
typedef Elf64_Phdr _Elf_Phdr;
typedef Elf64_Shdr _Elf_Shdr;
typedef Elf64_Sym  _Elf_Sym;
#define _ELF_CLASS   ELFCLASS64
#define _ELF_ST_TYPE ELF64_ST_TYPE
#else
typedef Elf32_Ehdr _Elf_Ehdr;
typedef Elf32_Phdr _Elf_Phdr;
typedef Elf32_Shdr _Elf_Shdr;
typedef Elf32_Sym  _Elf_Sym;
#define _ELF_CLASS   ELFCLASS32
#define _ELF_ST_TYPE ELF32_ST_TYPE
#endif

typedef struct {
    uintptr_t addr;     // runtime address (on-disk value + ASLR slide)
    uint32_t  name_off; // byte offset into linux_sym_table_t.strtab
} _linux_sym_t;

typedef struct {
    _linux_sym_t* entries;
    size_t        count;
    char*         strtab; // owned copy of the ELF string table
} linux_sym_table_t;

static hash_table_t*     _linux_sym_cache = NULL;
static linux_sym_table_t _linux_no_syms_sentinel;
#define _LINUX_NO_SYMS (&_linux_no_syms_sentinel)

static int
_linux_sym_cmp(const void* a, const void* b) {
    const _linux_sym_t* sa = (const _linux_sym_t*)a;
    const _linux_sym_t* sb = (const _linux_sym_t*)b;
    return (sa->addr > sb->addr) - (sa->addr < sb->addr);
}

// Compute the ASLR slide: (runtime load_base) - (first PT_LOAD p_vaddr).
// load_base comes from the first mapping address in /proc/pid/maps, which
// corresponds to the first PT_LOAD segment (p_vaddr is typically 0 for modern
// shared libraries).  Using the first *executable* PT_LOAD's p_vaddr is wrong:
// its p_vaddr > 0, producing a slide offset by -p_vaddr and breaking all
// symbol address comparisons.
static intptr_t
_linux_compute_slide(const void* map, size_t map_size, uintptr_t load_base) {
    const _Elf_Ehdr* ehdr  = (const _Elf_Ehdr*)map;
    const _Elf_Phdr* phdrs = (const _Elf_Phdr*)((const char*)map + ehdr->e_phoff);

    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(_Elf_Phdr) > map_size)
        return 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD)
            return (intptr_t)load_base - (intptr_t)phdrs[i].p_vaddr;
    }
    return 0;
}

// Build a sorted symbol table from a mapped ELF image.
static linux_sym_table_t*
_linux_build_sym_table(const void* map, size_t map_size, intptr_t slide) {
    const _Elf_Ehdr* ehdr = (const _Elf_Ehdr*)map;

    if (map_size < sizeof(_Elf_Ehdr))
        return NULL; // GCOV_EXCL_LINE
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 || ehdr->e_ident[EI_MAG2] != ELFMAG2
        || ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return NULL; // GCOV_EXCL_LINE
    if (ehdr->e_ident[EI_CLASS] != _ELF_CLASS)
        return NULL; // GCOV_EXCL_LINE

    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0)
        return NULL;
    if (ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(_Elf_Shdr) > map_size)
        return NULL; // GCOV_EXCL_LINE

    const _Elf_Shdr* shdrs    = (const _Elf_Shdr*)((const char*)map + ehdr->e_shoff);
    const _Elf_Shdr* sym_shdr = NULL;
    const _Elf_Shdr* str_shdr = NULL;

    // Prefer .symtab (full symbol table) over .dynsym (exported symbols only).
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type == SHT_SYMTAB && shdrs[i].sh_link < ehdr->e_shnum) {
            sym_shdr = &shdrs[i];
            str_shdr = &shdrs[shdrs[i].sh_link];
            break;
        }
    }
    if (!sym_shdr) {
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            if (shdrs[i].sh_type == SHT_DYNSYM && shdrs[i].sh_link < ehdr->e_shnum) {
                sym_shdr = &shdrs[i];
                str_shdr = &shdrs[shdrs[i].sh_link];
                break;
            }
        }
    }
    if (!sym_shdr || !str_shdr)
        return NULL;

    if (sym_shdr->sh_offset + sym_shdr->sh_size > map_size)
        return NULL; // GCOV_EXCL_LINE
    if (str_shdr->sh_offset + str_shdr->sh_size > map_size)
        return NULL; // GCOV_EXCL_LINE
    if (sym_shdr->sh_entsize < sizeof(_Elf_Sym))
        return NULL; // GCOV_EXCL_LINE

    const _Elf_Sym* elfsyms = (const _Elf_Sym*)((const char*)map + sym_shdr->sh_offset);
    size_t          nsyms   = (size_t)(sym_shdr->sh_size / sym_shdr->sh_entsize);
    const char*     strtab  = (const char*)map + str_shdr->sh_offset;
    size_t          strsize = (size_t)str_shdr->sh_size;

    // Count qualifying: STT_FUNC or STT_NOTYPE, defined, non-zero address, named.
    size_t count = 0;
    for (size_t i = 0; i < nsyms; i++) {
        unsigned char type = _ELF_ST_TYPE(elfsyms[i].st_info);
        if (type != STT_FUNC && type != STT_NOTYPE)
            continue;
        if (elfsyms[i].st_shndx == SHN_UNDEF || elfsyms[i].st_value == 0)
            continue;
        uint32_t noff = elfsyms[i].st_name;
        if (noff >= strsize || strtab[noff] == '\0')
            continue;
        count++;
    }

    if (count == 0)
        return NULL;

    _linux_sym_t* entries      = (_linux_sym_t*)malloc(count * sizeof(_linux_sym_t));
    char*         owned_strtab = (char*)malloc(strsize);
    if (!entries || !owned_strtab) { // GCOV_EXCL_START
        free(entries);
        free(owned_strtab);
        return NULL;
    } // GCOV_EXCL_STOP

    memcpy(owned_strtab, strtab, strsize);

    size_t idx = 0;
    for (size_t i = 0; i < nsyms; i++) {
        unsigned char type = _ELF_ST_TYPE(elfsyms[i].st_info);
        if (type != STT_FUNC && type != STT_NOTYPE)
            continue;
        if (elfsyms[i].st_shndx == SHN_UNDEF || elfsyms[i].st_value == 0)
            continue;
        uint32_t noff = elfsyms[i].st_name;
        if (noff >= strsize || strtab[noff] == '\0')
            continue;

        entries[idx].addr     = (uintptr_t)((intptr_t)elfsyms[i].st_value + slide);
        entries[idx].name_off = noff;
        idx++;
    }

    qsort(entries, count, sizeof(_linux_sym_t), _linux_sym_cmp);

    linux_sym_table_t* table = (linux_sym_table_t*)malloc(sizeof(linux_sym_table_t));
    if (!table) { // GCOV_EXCL_START
        free(entries);
        free(owned_strtab);
        return NULL;
    } // GCOV_EXCL_STOP

    table->entries = entries;
    table->count   = count;
    table->strtab  = owned_strtab;

    log_d("linux_addr2line: loaded %zu symbols (slide=%" PRIdPTR ")", count, slide);
    return table;
}

static linux_sym_table_t*
_linux_load_sym_table(const char* path, uintptr_t load_base) {
    cu_fd fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0)
        return NULL; // GCOV_EXCL_LINE
    if (st.st_size < (off_t)sizeof(Elf64_Ehdr))
        return NULL; // GCOV_EXCL_LINE

    cu_map_t* mapping = map_new(fd, (size_t)st.st_size, MAP_PRIVATE);
    if (!isvalid(mapping))
        return NULL; // GCOV_EXCL_LINE

    intptr_t slide = _linux_compute_slide(mapping->addr, (size_t)st.st_size, load_base);
    return _linux_build_sym_table(mapping->addr, (size_t)st.st_size, slide);
}

static const char*
_linux_lookup_sym(linux_sym_table_t* table, uintptr_t pc) {
    if (table->count == 0)
        return NULL;

    size_t lo = 0, hi = table->count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (table->entries[mid].addr <= pc)
            lo = mid;
        else
            hi = mid;
    }

    if (table->entries[lo].addr > pc)
        return NULL;

    return table->strtab + table->entries[lo].name_off;
}

// Returns a string owned by the cached symbol table, or NULL.
// load_base is the runtime base address of the binary's first executable
// segment, obtained from the vm_range_tree built from /proc/<pid>/maps.
static const char*
linux_get_func_name(uintptr_t pc, const char* path, uintptr_t load_base) {
    if (!isvalid(_linux_sym_cache)) {
        _linux_sym_cache = hash_table_new(256);
        if (!isvalid(_linux_sym_cache))
            return NULL;
    }

    key_dt             key   = (key_dt)string__hash((char*)path);
    linux_sym_table_t* table = (linux_sym_table_t*)hash_table__get(_linux_sym_cache, key);

    if (!isvalid(table)) {
        table = _linux_load_sym_table(path, load_base);
        hash_table__set(_linux_sym_cache, key, (value_t)(table ? table : _LINUX_NO_SYMS));
        if (!isvalid(table))
            return NULL;
    }

    if (table == _LINUX_NO_SYMS)
        return NULL;

    return _linux_lookup_sym(table, pc);
}

#endif /* AUSTINP || x86_64 || aarch64 */
