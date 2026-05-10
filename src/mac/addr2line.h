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

// Mach-O native symbol resolution for austin on macOS.
//
// Provides get_func_name() which maps a program counter to the enclosing
// function name by reading the binary's Mach-O LC_SYMTAB, computing the ASLR
// slide, and doing a binary search in the sorted symbol table.
//
// For binaries that exist on disk (Python itself, user code) the Mach-O file
// is opened directly.  For system frameworks that live exclusively inside the
// dyld shared cache (CoreFoundation, Security, libSystem, etc.) we fall back
// to parsing the cache — see dyld_cache.h.
//
// Results are cached per binary path so the expensive IO and region scans
// happen at most once per unique loaded binary.

#pragma once

#include <fcntl.h>
#include <libproc.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include "../cache.h"
#include "../logging.h"
#include "../py_string.h"
#include "../resources.h"

#include "dyld_cache.h"

// One entry in a sorted symbol table.
typedef struct {
    uintptr_t addr;     // runtime address
    uint32_t  name_off; // byte offset into sym_table_t.strtab
} _sym_t;

// Symbol table for one binary, sorted by runtime address.
typedef struct {
    _sym_t* entries;
    size_t  count;
    char*   strtab; // owned copy of the Mach-O string table
} sym_table_t;

// File-scope cache: string hash of binary path → sym_table_t*
// _NO_SYMS is stored when a binary was opened but yielded no symbols
// (e.g. stripped, shared-cache stub, unsupported format).
static hash_table_t* _sym_cache = NULL;
static sym_table_t   _no_syms_sentinel;
#define _NO_SYMS (&_no_syms_sentinel)

// ---- qsort comparator -------------------------------------------------------
static int
_sym_cmp(const void* a, const void* b) {
    const _sym_t* sa = (const _sym_t*)a;
    const _sym_t* sb = (const _sym_t*)b;
    return (sa->addr > sb->addr) - (sa->addr < sb->addr);
}

// ---- Find the runtime load base for a binary --------------------------------
// Returns the runtime load address of the binary's __TEXT segment.
// For shared-cache images the load base is computed directly from the cache
// metadata, avoiding the expensive VM-region scan entirely.
static uintptr_t
_find_load_base(mach_port_t task, pid_t pid, const char* target_path) {
    // Fast path: check the dyld shared cache first.  System frameworks only
    // exist inside the cache on modern macOS, so the VM-region scan below
    // would iterate every region (hundreds of Mach traps) and still fail.
    _dsc_t* dsc = _dsc_get();
    if (dsc) {
        uint64_t image_va = _dsc_find_image(dsc, target_path);
        if (image_va != 0) {
            uintptr_t slide = _dsc_get_slide(task);
            uintptr_t base  = (uintptr_t)(image_va + slide);
            log_d("addr2line: load base for %s from cache: %p (slide=0x%" PRIxPTR ")", target_path, (void*)base, slide);
            return base;
        }
    }

    // Slow path: scan VM regions (for binaries that exist on disk).
    mach_vm_address_t              addr  = 1;
    mach_vm_size_t                 size  = 0;
    mach_msg_type_number_t         count = sizeof(vm_region_basic_info_data_64_t);
    vm_region_basic_info_data_64_t info  = {0};
    mach_port_t                    obj;
    char                           path[MAXPATHLEN + 1];

    while (mach_vm_region(task, &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj)
           == KERN_SUCCESS) {
        if (info.protection & VM_PROT_EXECUTE) {
            int len = proc_regionfilename(pid, addr, path, MAXPATHLEN);
            if (len > 0 && strcmp(path, target_path) == 0) {
                log_d("addr2line: load base for %s is %p", target_path, (void*)addr);
                return (uintptr_t)addr;
            }
        }
        addr += size;
    }

    log_d("addr2line: could not find load base for %s", target_path);
    return 0;
}

// ---- Common: filter, sort and package nlist entries -------------------------
// Takes raw pointers to a Mach-O nlist table and its string table, applies
// the ASLR slide, and returns a freshly allocated sym_table_t.
static sym_table_t*
_build_sym_table(const struct nlist_64* sym_tab, uint32_t nsyms, const char* str_raw, uint32_t strsize, int64_t slide) {
    // Count qualifying symbols: non-stab, defined in a section, non-empty name.
    size_t count = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        uint8_t type = sym_tab[i].n_type;
        if (type & N_STAB)
            continue;
        if ((type & N_TYPE) != N_SECT)
            continue;
        if (sym_tab[i].n_sect == NO_SECT || sym_tab[i].n_value == 0)
            continue;
        uint32_t strx = sym_tab[i].n_un.n_strx;
        if (strx >= strsize || str_raw[strx] == '\0')
            continue;
        count++;
    }

    if (count == 0)
        return NULL;

    _sym_t* entries = (_sym_t*)malloc(count * sizeof(_sym_t));
    char*   strtab  = (char*)malloc(strsize);
    if (!entries || !strtab) {
        free(entries);
        free(strtab);
        return NULL;
    }

    memcpy(strtab, str_raw, strsize);

    size_t idx = 0;
    for (uint32_t i = 0; i < nsyms; i++) {
        uint8_t type = sym_tab[i].n_type;
        if (type & N_STAB)
            continue;
        if ((type & N_TYPE) != N_SECT)
            continue;
        if (sym_tab[i].n_sect == NO_SECT || sym_tab[i].n_value == 0)
            continue;
        uint32_t strx = sym_tab[i].n_un.n_strx;
        if (strx >= strsize || str_raw[strx] == '\0')
            continue;

        entries[idx].addr     = (uintptr_t)((int64_t)sym_tab[i].n_value + slide);
        entries[idx].name_off = strx;
        idx++;
    }

    qsort(entries, count, sizeof(_sym_t), _sym_cmp);

    sym_table_t* table = (sym_table_t*)malloc(sizeof(sym_table_t));
    if (!table) {
        free(entries);
        free(strtab);
        return NULL;
    }

    table->entries = entries;
    table->count   = count;
    table->strtab  = strtab;
    return table;
}

// ---- Parse a Mach-O 64-bit image from a flat file ---------------------------
// map      – pointer to the start of the Mach-O image (mmapped file or slice)
// load_base – runtime load address of the binary's __TEXT segment
static sym_table_t*
_build_table64(void* map, uintptr_t load_base) {
    struct mach_header_64* hdr = (struct mach_header_64*)map;

    if (hdr->filetype != MH_EXECUTE && hdr->filetype != MH_DYLIB && hdr->filetype != MH_BUNDLE)
        return NULL;

    int64_t  slide     = 0;
    uint32_t symoff    = 0;
    uint32_t nsyms     = 0;
    uint32_t stroff    = 0;
    uint32_t strsize   = 0;
    bool     has_slide = false;

    uint32_t ncmds = hdr->ncmds;
    void*    lc    = (char*)map + sizeof(struct mach_header_64);

    for (uint32_t i = 0; i < ncmds; i++) {
        struct load_command* cmd = (struct load_command*)lc;

        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            struct segment_command_64* seg = (struct segment_command_64*)lc;
            if (strcmp(seg->segname, "__TEXT") == 0) {
                slide     = (int64_t)load_base - (int64_t)seg->vmaddr;
                has_slide = true;
            }
            break;
        }
        case LC_SYMTAB: {
            struct symtab_command* sc = (struct symtab_command*)lc;
            symoff                    = sc->symoff;
            nsyms                     = sc->nsyms;
            stroff                    = sc->stroff;
            strsize                   = sc->strsize;
            break;
        }
        }

        lc = (char*)lc + cmd->cmdsize;
    }

    if (!has_slide || nsyms == 0 || strsize == 0)
        return NULL;

    const struct nlist_64* sym_tab = (const struct nlist_64*)((char*)map + symoff);
    const char*            str_raw = (const char*)map + stroff;

    sym_table_t* table = _build_sym_table(sym_tab, nsyms, str_raw, strsize, slide);
    if (table)
        log_d("addr2line: loaded %zu symbols from file (slide=%" PRIdPTR ")", table->count, (intptr_t)slide);
    return table;
}

// ---- Parse a Mach-O 64-bit image from the dyld shared cache -----------------
// dsc       – loaded shared cache
// image_va  – un-slid VM address of the image's Mach-O header in the cache
// load_base – runtime load address of the binary's __TEXT segment in the target
static sym_table_t*
_build_table64_from_cache(const _dsc_t* dsc, uint64_t image_va, uintptr_t load_base) {
    void* hdr_raw = _dsc_translate(dsc, image_va);
    if (!hdr_raw)
        return NULL;

    struct mach_header_64* hdr = (struct mach_header_64*)hdr_raw;
    if (hdr->magic != MH_MAGIC_64)
        return NULL;
    if (hdr->filetype != MH_EXECUTE && hdr->filetype != MH_DYLIB && hdr->filetype != MH_BUNDLE)
        return NULL;

    int64_t  slide            = 0;
    uint32_t symoff           = 0;
    uint32_t nsyms            = 0;
    uint32_t stroff           = 0;
    uint32_t strsize          = 0;
    uint64_t linkedit_vmaddr  = 0;
    uint64_t linkedit_fileoff = 0;
    bool     has_slide        = false;
    bool     has_linkedit     = false;

    uint32_t ncmds = hdr->ncmds;
    void*    lc    = (char*)hdr_raw + sizeof(struct mach_header_64);

    for (uint32_t i = 0; i < ncmds; i++) {
        struct load_command* cmd = (struct load_command*)lc;

        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            struct segment_command_64* seg = (struct segment_command_64*)lc;
            if (strcmp(seg->segname, "__TEXT") == 0) {
                slide     = (int64_t)load_base - (int64_t)seg->vmaddr;
                has_slide = true;
            } else if (strcmp(seg->segname, "__LINKEDIT") == 0) {
                linkedit_vmaddr  = seg->vmaddr;
                linkedit_fileoff = seg->fileoff;
                has_linkedit     = true;
            }
            break;
        }
        case LC_SYMTAB: {
            struct symtab_command* sc = (struct symtab_command*)lc;
            symoff                    = sc->symoff;
            nsyms                     = sc->nsyms;
            stroff                    = sc->stroff;
            strsize                   = sc->strsize;
            break;
        }
        }

        lc = (char*)lc + cmd->cmdsize;
    }

    if (!has_slide || !has_linkedit || nsyms == 0 || strsize == 0)
        return NULL;

    // In the cache, symoff/stroff are file offsets relative to the original
    // binary layout.  Translate them to VM addresses via the __LINKEDIT
    // segment, then resolve through the cache mapping table.
    uint64_t nlist_va  = linkedit_vmaddr + (symoff - linkedit_fileoff);
    uint64_t strtab_va = linkedit_vmaddr + (stroff - linkedit_fileoff);

    const struct nlist_64* sym_tab = (const struct nlist_64*)_dsc_translate(dsc, nlist_va);
    const char*            str_raw = (const char*)_dsc_translate(dsc, strtab_va);

    if (!sym_tab || !str_raw) {
        log_d("addr2line: cache LINKEDIT translation failed");
        return NULL;
    }

    sym_table_t* table = _build_sym_table(sym_tab, nsyms, str_raw, strsize, slide);
    if (table)
        log_d("addr2line: loaded %zu symbols from dyld cache (slide=%" PRIdPTR ")", table->count, (intptr_t)slide);
    return table;
}

// ---- Fat binary dispatcher --------------------------------------------------
static sym_table_t*
_build_table_fat(void* map, uintptr_t load_base) {
    cpu_type_t cpu;
    int        is_abi64;
    size_t     cpu_sz   = sizeof(cpu);
    size_t     abi64_sz = sizeof(is_abi64);

    sysctlbyname("hw.cputype", &cpu, &cpu_sz, NULL, 0);
    sysctlbyname("hw.cpu64bit_capable", &is_abi64, &abi64_sz, NULL, 0);
    cpu |= is_abi64 * CPU_ARCH_ABI64;

    struct fat_header* fat   = (struct fat_header*)map;
    struct fat_arch*   archs = (struct fat_arch*)((char*)map + sizeof(struct fat_header));
    uint32_t           n     = OSSwapBigToHostInt32(fat->nfat_arch);

    for (uint32_t i = 0; i < n; i++) {
        if ((cpu_type_t)OSSwapBigToHostInt32((uint32_t)archs[i].cputype) == cpu) {
            void*                  slice = (char*)map + OSSwapBigToHostInt32(archs[i].offset);
            struct mach_header_64* hdr   = (struct mach_header_64*)slice;
            if (hdr->magic == MH_MAGIC_64 || hdr->magic == MH_CIGAM_64)
                return _build_table64(slice, load_base);
            return NULL; // 32-bit or unknown – not supported
        }
    }

    return NULL;
}

// ---- Try the dyld shared cache as a fallback --------------------------------
static sym_table_t*
_load_sym_table_from_cache(const char* path, uintptr_t load_base) {
    _dsc_t* dsc = _dsc_get();
    if (!dsc)
        return NULL;

    uint64_t image_va = _dsc_find_image(dsc, path);
    if (image_va == 0) {
        log_d("addr2line: %s not found in dyld cache", path);
        return NULL;
    }

    log_d("addr2line: resolving %s from dyld cache (image VA %p)", path, (void*)image_va);
    return _build_table64_from_cache(dsc, image_va, load_base);
}

// ---- Open binary file and dispatch to parser --------------------------------
static sym_table_t*
_load_sym_table(const char* path, uintptr_t load_base) {
    cu_fd fd = open(path, O_RDONLY);
    if (fd < 0)
        // File not on disk — try the dyld shared cache (system frameworks on
        // modern macOS only exist inside the cache).
        return _load_sym_table_from_cache(path, load_base);

    struct stat st;
    if (fstat(fd, &st) < 0)
        return NULL;

    if (st.st_size < (off_t)sizeof(struct mach_header))
        return NULL;

    cu_map_t* mapping = map_new(fd, (size_t)st.st_size, MAP_PRIVATE);
    if (!isvalid(mapping))
        return NULL;

    void*                  map = mapping->addr;
    struct mach_header_64* hdr = (struct mach_header_64*)map;

    switch (hdr->magic) {
    case MH_MAGIC_64:
    case MH_CIGAM_64:
        return _build_table64(map, load_base);
    case FAT_MAGIC:
    case FAT_CIGAM:
        return _build_table_fat(map, load_base);
    default:
        return NULL;
    }
}

// ---- Binary search ----------------------------------------------------------
static const char*
_lookup_sym(sym_table_t* table, uintptr_t pc) {
    if (table->count == 0)
        return NULL;

    // Find the greatest entry whose address is <= pc.
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

    const char* name = table->strtab + table->entries[lo].name_off;
    // Strip the leading underscore that the Mach-O ABI adds to C symbols.
    if (name[0] == '_')
        name++;
    return name;
}

// ---- Public API -------------------------------------------------------------

// Look up the function name that contains pc in the binary at path.
// task and pid are from the target process (proc->ref and proc->pid).
// Returns a pointer to a string owned by the cached symbol table, or NULL.
static const char*
get_func_name(mach_port_t task, pid_t pid, uintptr_t pc, const char* path) {
    if (!isvalid(_sym_cache)) {
        _sym_cache = hash_table_new(256);
        if (!isvalid(_sym_cache))
            return NULL;
    }

    key_dt       key   = (key_dt)string__hash((char*)path);
    sym_table_t* table = (sym_table_t*)hash_table__get(_sym_cache, key);

    if (!isvalid(table)) {
        // Not yet cached: locate load base and build the symbol table.
        uintptr_t load_base = _find_load_base(task, pid, path);
        if (load_base == 0) {
            hash_table__set(_sym_cache, key, (value_t)_NO_SYMS);
            return NULL;
        }

        table = _load_sym_table(path, load_base);
        hash_table__set(_sym_cache, key, (value_t)(table ? table : _NO_SYMS));

        if (!isvalid(table))
            return NULL;
    }

    if (table == _NO_SYMS)
        return NULL;

    return _lookup_sym(table, pc);
}
