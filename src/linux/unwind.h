// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2026 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

// Minimal DWARF CFI (.eh_frame) stack unwinder for plain austin on Linux.
//
// When a binary is compiled without frame pointers (-fomit-frame-pointer,
// the default in GCC/Clang -O2+) the rbp/x29 chain is absent.  Every
// non-stripped binary still carries a .eh_frame section needed for C++
// exception handling — this section encodes, for each PC range, how to
// recover the Canonical Frame Address (CFA) and the return address.
//
// This file implements a self-contained reader that:
//   1. Locates the .eh_frame section in a mmap'd ELF image.
//   2. Scans CIE/FDE records to find the FDE whose range covers `pc`.
//   3. Evaluates the restricted set of DW_CFA_* opcodes used by GCC/Clang
//      to compute (cfa_reg, cfa_offset, ra_offset).
//   4. Exposes cfi_step() which, given the current (pc, sp, fp) of a
//      ptrace-stopped thread and a process_vm_readv accessor, produces
//      the caller's (pc, sp).
//
// Only the opcodes actually emitted by GCC/Clang for x86-64 and aarch64 are
// handled.  Complex expressions and rarely-used opcodes fall through to
// cfi_step() returning false, at which point the caller gives up.

#pragma once

#if defined(PL_LINUX) && !defined(AUSTINP) && (defined(__x86_64__) || defined(__aarch64__))

#include <elf.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "../logging.h"
#include "../resources.h"
#include "vm-range-tree.h"

// ---------------------------------------------------------------------------
// DWARF encoding constants
// ---------------------------------------------------------------------------

// CFA definition opcodes
#define DW_CFA_nop                0x00
#define DW_CFA_set_loc            0x01
#define DW_CFA_advance_loc1       0x02
#define DW_CFA_advance_loc2       0x03
#define DW_CFA_advance_loc3       0x04
#define DW_CFA_offset_extended    0x05
#define DW_CFA_restore_extended   0x06
#define DW_CFA_undefined          0x07
#define DW_CFA_same_value         0x08
#define DW_CFA_register           0x09
#define DW_CFA_remember_state     0x0a
#define DW_CFA_restore_state      0x0b
#define DW_CFA_def_cfa            0x0c
#define DW_CFA_def_cfa_register   0x0d
#define DW_CFA_def_cfa_offset     0x0e
#define DW_CFA_def_cfa_expression 0x0f
#define DW_CFA_expression         0x10
#define DW_CFA_offset_extended_sf 0x11
#define DW_CFA_def_cfa_sf         0x12
#define DW_CFA_def_cfa_offset_sf  0x13
#define DW_CFA_val_offset         0x14
#define DW_CFA_val_offset_sf      0x15
#define DW_CFA_val_expression     0x16
#define DW_CFA_lo_user            0x1c
#define DW_CFA_GNU_args_size      0x2e
#define DW_CFA_hi_user            0x3f

// High-2-bit opcodes
#define DW_CFA_advance_loc 0x40 // operand = delta in low 6 bits
#define DW_CFA_offset      0x80 // operand = register in low 6 bits
#define DW_CFA_restore     0xc0 // operand = register in low 6 bits

// .eh_frame pointer encodings
#define DW_EH_PE_omit     0xff
#define DW_EH_PE_absptr   0x00
#define DW_EH_PE_uleb128  0x01
#define DW_EH_PE_udata2   0x02
#define DW_EH_PE_udata4   0x03
#define DW_EH_PE_udata8   0x04
#define DW_EH_PE_sleb128  0x09
#define DW_EH_PE_sdata2   0x0a
#define DW_EH_PE_sdata4   0x0b
#define DW_EH_PE_sdata8   0x0c
#define DW_EH_PE_pcrel    0x10
#define DW_EH_PE_datarel  0x20
#define DW_EH_PE_funcrel  0x40
#define DW_EH_PE_indirect 0x80

// Return-address register numbers (DWARF)
#if defined(__x86_64__)
#define _CFI_RA_REG 16 // x86-64: column 16 = return address
#define _CFI_SP_REG 7  // RSP
#define _CFI_FP_REG 6  // RBP
#elif defined(__aarch64__)
#define _CFI_RA_REG 30 // aarch64: x30 = link register
#define _CFI_SP_REG 31 // SP
#define _CFI_FP_REG 29 // x29 frame pointer
#endif

// Maximum number of DWARF register columns we track.
// x86-64 uses up to column 17; aarch64 up to column 31.
#define _CFI_MAX_REGS 32

// ---------------------------------------------------------------------------
// Internal types
// ---------------------------------------------------------------------------

// How a register's value is recovered from the caller's frame.
typedef enum {
    REG_UNDEF,      // not saved / don't care
    REG_SAME,       // unchanged from callee
    REG_CFA_OFFSET, // *(CFA + offset)
} cfi_reg_rule_t;

typedef struct {
    cfi_reg_rule_t kind;
    int64_t        offset; // used when kind == REG_CFA_OFFSET
} cfi_reg_t;

// A parsed CFI row: CFA = reg + offset, plus per-register save rules.
typedef struct {
    uint8_t   cfa_reg;
    int64_t   cfa_off;
    cfi_reg_t regs[_CFI_MAX_REGS];
} cfi_row_t;

// Pre-parsed FDE descriptor for the sorted index.
typedef struct {
    uintptr_t      pc_begin;
    uintptr_t      pc_end;
    const uint8_t* cie_ptr;    // pointer to the CIE record in the mmap'd data
    const uint8_t* fde_instrs; // start of FDE instructions (after header)
    const uint8_t* fde_end;    // end of FDE record
} cfi_fde_t;

// Per-binary .eh_frame cache entry.
typedef struct _cfi_cache_entry {
    uint64_t                 key;      // string hash of the binary path
    const uint8_t*           data;     // pointer into the mmap'd file
    size_t                   size;     // byte size of .eh_frame
    uintptr_t                sec_addr; // runtime VA of the section start
    intptr_t                 slide;    // ASLR slide applied to FDE pc values
    void*                    map;      // mmap base (kept alive)
    size_t                   map_size;
    cfi_fde_t*               fde_index; // sorted array of FDE descriptors
    size_t                   fde_count; // number of entries in fde_index
    struct _cfi_cache_entry* next;
} cfi_cache_entry_t;

#define _CFI_CACHE_BUCKETS 64
static cfi_cache_entry_t* _cfi_cache[_CFI_CACHE_BUCKETS];

// ---------------------------------------------------------------------------
// LEB128 readers
// ---------------------------------------------------------------------------

static inline uint64_t
_read_uleb128(const uint8_t** p, const uint8_t* end) {
    uint64_t val   = 0;
    unsigned shift = 0;
    while (*p < end) {
        uint8_t b  = *(*p)++;
        val       |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            break;
        shift += 7;
    }
    return val;
}

static inline int64_t
_read_sleb128(const uint8_t** p, const uint8_t* end) {
    int64_t  val   = 0;
    unsigned shift = 0;
    uint8_t  b     = 0;
    while (*p < end) {
        b      = *(*p)++;
        val   |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift < 64 && (b & 0x40))
        val |= -(int64_t)(1ull << shift);
    return val;
}

static inline uint16_t
_read_u16(const uint8_t** p) {
    uint16_t v;
    memcpy(&v, *p, 2);
    *p += 2;
    return v;
}
static inline uint32_t
_read_u32(const uint8_t** p) {
    uint32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}
static inline uint64_t
_read_u64(const uint8_t** p) {
    uint64_t v;
    memcpy(&v, *p, 8);
    *p += 8;
    return v;
}
static inline int32_t
_read_s32(const uint8_t** p) {
    int32_t v;
    memcpy(&v, *p, 4);
    *p += 4;
    return v;
}

// ---------------------------------------------------------------------------
// Pointer-encoding reader (simplified: pcrel and absptr only)
// ---------------------------------------------------------------------------

static inline uintptr_t
_read_encoded_ptr(const uint8_t** p, const uint8_t* end, uint8_t enc, uintptr_t pc_base) {
    if (enc == DW_EH_PE_omit)
        return 0;

    uintptr_t result = 0;

    switch (enc & 0x0f) {
    case DW_EH_PE_absptr:
        if (*p + sizeof(uintptr_t) > end)
            return 0;
        memcpy(&result, *p, sizeof(uintptr_t));
        *p += sizeof(uintptr_t);
        break;
    case DW_EH_PE_udata4:
        if (*p + 4 > end)
            return 0;
        result = _read_u32(p);
        break;
    case DW_EH_PE_udata8:
        if (*p + 8 > end)
            return 0;
        result = (uintptr_t)_read_u64(p);
        break;
    case DW_EH_PE_sdata4:
        if (*p + 4 > end)
            return 0;
        result = (uintptr_t)(intptr_t)_read_s32(p);
        break;
    case DW_EH_PE_sleb128:
        result = (uintptr_t)_read_sleb128(p, end);
        break;
    case DW_EH_PE_uleb128:
        result = (uintptr_t)_read_uleb128(p, end);
        break;
    default:
        return 0; // unsupported encoding
    }

    if (enc & DW_EH_PE_pcrel)
        result += pc_base;

    return result;
}

// ---------------------------------------------------------------------------
// CFI opcode interpreter
// ---------------------------------------------------------------------------

// Evaluate CFI opcodes in [p, end) advancing the PC from `row_pc` to
// `target_pc`, updating `row` as we go.  Returns true if we consumed
// opcodes up to (or past) target_pc without error.
static bool
_cfi_eval(
    const uint8_t* p, const uint8_t* end, uintptr_t row_pc, uintptr_t target_pc, uint64_t code_align,
    int64_t data_align, cfi_row_t* row, const cfi_row_t* initial_row
) {
    cfi_row_t state_stack[8];
    int       state_depth = 0;

    while (p < end && row_pc <= target_pc) {
        uint8_t op = *p++;

        if ((op & 0xc0) == DW_CFA_advance_loc) {
            row_pc += (op & 0x3f) * code_align;
            continue;
        }
        if ((op & 0xc0) == DW_CFA_offset) {
            uint8_t  reg = op & 0x3f;
            uint64_t off = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS) {
                row->regs[reg].kind   = REG_CFA_OFFSET;
                row->regs[reg].offset = (int64_t)off * data_align;
            }
            continue;
        }
        if ((op & 0xc0) == DW_CFA_restore) {
            uint8_t reg = op & 0x3f;
            if (reg < _CFI_MAX_REGS) {
                if (initial_row)
                    row->regs[reg] = initial_row->regs[reg];
                else
                    row->regs[reg].kind = REG_UNDEF;
            }
            continue;
        }

        switch (op) {
        case DW_CFA_nop:
            break;

        case DW_CFA_def_cfa: {
            uint64_t reg = _read_uleb128(&p, end);
            uint64_t off = _read_uleb128(&p, end);
            row->cfa_reg = (uint8_t)reg;
            row->cfa_off = (int64_t)off;
            break;
        }
        case DW_CFA_def_cfa_register: {
            uint64_t reg = _read_uleb128(&p, end);
            row->cfa_reg = (uint8_t)reg;
            break;
        }
        case DW_CFA_def_cfa_offset: {
            uint64_t off = _read_uleb128(&p, end);
            row->cfa_off = (int64_t)off;
            break;
        }
        case DW_CFA_def_cfa_sf: { // GCOV_EXCL_START
            uint64_t reg = _read_uleb128(&p, end);
            int64_t  off = _read_sleb128(&p, end);
            row->cfa_reg = (uint8_t)reg;
            row->cfa_off = off * data_align;
            break;
        }
        case DW_CFA_def_cfa_offset_sf: {
            int64_t off  = _read_sleb128(&p, end);
            row->cfa_off = off * data_align;
            break;
        } // GCOV_EXCL_STOP

        case DW_CFA_offset_extended: { // GCOV_EXCL_START
            uint64_t reg = _read_uleb128(&p, end);
            uint64_t off = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS) {
                row->regs[reg].kind   = REG_CFA_OFFSET;
                row->regs[reg].offset = (int64_t)off * data_align;
            }
            break;
        }
        case DW_CFA_offset_extended_sf: {
            uint64_t reg = _read_uleb128(&p, end);
            int64_t  off = _read_sleb128(&p, end);
            if (reg < _CFI_MAX_REGS) {
                row->regs[reg].kind   = REG_CFA_OFFSET;
                row->regs[reg].offset = off * data_align;
            }
            break;
        } // GCOV_EXCL_STOP

        case DW_CFA_same_value: {
            uint64_t reg = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS)
                row->regs[reg].kind = REG_SAME;
            break;
        }
        case DW_CFA_undefined: {
            uint64_t reg = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS)
                row->regs[reg].kind = REG_UNDEF;
            break;
        }
        case DW_CFA_restore_extended: { // GCOV_EXCL_START
            uint64_t reg = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS) {
                if (initial_row)
                    row->regs[reg] = initial_row->regs[reg];
                else
                    row->regs[reg].kind = REG_UNDEF;
            }
            break;
        }
        case DW_CFA_register: {
            // reg1 = reg2 — uncommon, skip both operands
            _read_uleb128(&p, end);
            _read_uleb128(&p, end);
            break;
        } // GCOV_EXCL_STOP

        case DW_CFA_advance_loc1:
            if (p + 1 > end)
                return false; // GCOV_EXCL_LINE
            row_pc += *p++ * code_align;
            break;
        case DW_CFA_advance_loc2:
            if (p + 2 > end)
                return false; // GCOV_EXCL_LINE
            row_pc += _read_u16(&p) * code_align;
            break;
        case DW_CFA_advance_loc3:
            if (p + 4 > end)
                return false; // GCOV_EXCL_LINE
            row_pc += _read_u32(&p) * code_align;
            break;
        case DW_CFA_set_loc: { // GCOV_EXCL_START
            // New row_pc is an encoded address; we use absptr for simplicity.
            uintptr_t loc;
            if (p + sizeof(uintptr_t) > end)
                return false;
            memcpy(&loc, p, sizeof(uintptr_t));
            p      += sizeof(uintptr_t);
            row_pc  = loc;
            break;
        } // GCOV_EXCL_STOP

        case DW_CFA_remember_state:
            if (state_depth < 8)
                state_stack[state_depth++] = *row;
            break;
        case DW_CFA_restore_state:
            if (state_depth > 0)
                *row = state_stack[--state_depth];
            break;

        // Skip size argument for GNU_args_size.
        case DW_CFA_GNU_args_size:
            _read_uleb128(&p, end);
            break;

        // DWARF expressions: skip by reading block length then skipping.
        case DW_CFA_def_cfa_expression: // GCOV_EXCL_START
        case DW_CFA_expression:
        case DW_CFA_val_expression: {
            if (op == DW_CFA_expression || op == DW_CFA_val_expression)
                _read_uleb128(&p, end); // register operand
            uint64_t len = _read_uleb128(&p, end);
            if (p + len > end)
                return false;
            p += len;
            break;
        }

        default:
            // Unknown opcode — give up.
            return false; // GCOV_EXCL_STOP
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// .eh_frame loader — locates and caches the section for a binary
// ---------------------------------------------------------------------------

static cfi_cache_entry_t*
_cfi_load(const char* path, uintptr_t load_base, pid_t pid) {
    uint64_t key    = (uint64_t)string__hash((char*)path);
    unsigned bucket = (unsigned)(key % _CFI_CACHE_BUCKETS);

    for (cfi_cache_entry_t* e = _cfi_cache[bucket]; e; e = e->next) {
        if (e->key == key)
            return (e->data ? e : NULL); // NULL data = "no .eh_frame" sentinel
    }

    // Allocate sentinel so we don't re-scan on every miss.
    cfi_cache_entry_t* entry = (cfi_cache_entry_t*)calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;
    entry->key         = key;
    entry->next        = _cfi_cache[bucket];
    _cfi_cache[bucket] = entry;

    void*  map      = MAP_FAILED;
    size_t map_size = 0;

    if (strcmp(path, "[vdso]") == 0) {
        // The vDSO is a kernel-mapped ELF in the target process's address
        // space; there is no file to open.  Read the ELF header to determine
        // the total size, then copy the whole image into an anonymous mapping.
        _Elf_Ehdr    ehdr = {0};
        struct iovec lh   = {.iov_base = &ehdr, .iov_len = sizeof(ehdr)};
        struct iovec rh   = {.iov_base = (void*)load_base, .iov_len = sizeof(ehdr)};
        if (process_vm_readv(pid, &lh, 1, &rh, 1, 0) != (ssize_t)sizeof(ehdr))
            return NULL; // GCOV_EXCL_LINE
        if (ehdr.e_ident[EI_MAG0] != ELFMAG0 || ehdr.e_ident[EI_CLASS] != _ELF_CLASS)
            return NULL; // GCOV_EXCL_LINE

        // Section headers sit at the end of the image.
        map_size = (size_t)(ehdr.e_shoff + (uint64_t)ehdr.e_shnum * ehdr.e_shentsize);
        if (map_size < sizeof(ehdr))
            map_size = sizeof(ehdr); // GCOV_EXCL_LINE

        map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map == MAP_FAILED)
            return NULL; // GCOV_EXCL_LINE

        struct iovec lb = {.iov_base = map, .iov_len = map_size};
        struct iovec rb = {.iov_base = (void*)load_base, .iov_len = map_size};
        if (process_vm_readv(pid, &lb, 1, &rb, 1, 0) != (ssize_t)map_size) { // GCOV_EXCL_START
            munmap(map, map_size);
            return NULL;
        } // GCOV_EXCL_STOP
    } else {
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return NULL;

        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) { // GCOV_EXCL_START
            close(fd);
            return NULL;
        } // GCOV_EXCL_STOP

        map_size = (size_t)st.st_size;
        map      = mmap(NULL, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED)
            return NULL; // GCOV_EXCL_LINE
    }

    // Compute ASLR slide from the first PT_LOAD segment.
    // load_base comes from the first mapping address in /proc/pid/maps, which
    // corresponds to the first PT_LOAD segment (p_vaddr is typically 0 for
    // modern shared libraries).  Using the first *executable* PT_LOAD's p_vaddr
    // is wrong: its p_vaddr > 0, producing a slide offset by -p_vaddr and
    // breaking all FDE pc_begin comparisons.
    const _Elf_Ehdr* ehdr  = (const _Elf_Ehdr*)map;
    const _Elf_Phdr* phdrs = (const _Elf_Phdr*)((const char*)map + ehdr->e_phoff);

    intptr_t slide = 0;
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(_Elf_Phdr) <= map_size) {
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type == PT_LOAD) {
                slide = (intptr_t)load_base - (intptr_t)phdrs[i].p_vaddr;
                break;
            }
        }
    }

    // Try PT_GNU_EH_FRAME program header first — always present in the binary's
    // program header table, works even for stripped binaries that have no section headers.
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * sizeof(_Elf_Phdr) <= map_size) {
        for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
            if (phdrs[i].p_type != PT_GNU_EH_FRAME)
                continue;
            if (phdrs[i].p_filesz < 4)
                continue;
            if ((uint64_t)phdrs[i].p_offset + phdrs[i].p_filesz > (uint64_t)map_size)
                continue;

            const uint8_t* hdr_data = (const uint8_t*)map + phdrs[i].p_offset;
            const uint8_t* hdr_end  = hdr_data + phdrs[i].p_filesz;

            if (hdr_data[0] != 1)
                continue; // version must be 1
            uint8_t eh_frame_ptr_enc = hdr_data[1];
            if (eh_frame_ptr_enc == DW_EH_PE_omit)
                continue;

            // pc_base: runtime VA of the eh_frame_ptr field (4 bytes into the header).
            uintptr_t      pc_base     = (uintptr_t)((intptr_t)phdrs[i].p_vaddr + slide) + 4;
            const uint8_t* p           = hdr_data + 4;
            uintptr_t      eh_frame_va = _read_encoded_ptr(&p, hdr_end, eh_frame_ptr_enc, pc_base);
            if (!eh_frame_va)
                continue;

            // Convert runtime VA of .eh_frame to a file offset via PT_LOAD segments.
            uintptr_t eh_frame_static_va = (uintptr_t)((intptr_t)eh_frame_va - slide);
            uintptr_t file_off           = 0;
            for (uint16_t j = 0; j < ehdr->e_phnum; j++) {
                if (phdrs[j].p_type != PT_LOAD)
                    continue;
                if (eh_frame_static_va >= (uintptr_t)phdrs[j].p_vaddr
                    && eh_frame_static_va < (uintptr_t)(phdrs[j].p_vaddr + phdrs[j].p_filesz)) {
                    file_off = (uintptr_t)phdrs[j].p_offset + (eh_frame_static_va - (uintptr_t)phdrs[j].p_vaddr);
                    break;
                }
            }
            if (!file_off || file_off >= (uintptr_t)map_size)
                continue;

            entry->data     = (const uint8_t*)map + file_off;
            entry->size     = map_size - (size_t)file_off;
            entry->sec_addr = eh_frame_va;
            entry->slide    = slide;
            entry->map      = map;
            entry->map_size = map_size;

            log_d(
                "cfi: loaded .eh_frame for %s via PT_GNU_EH_FRAME (%zu bytes, slide=%" PRIdPTR ")", path, entry->size,
                slide
            );
            return entry;
        }
    }

    // Fallback: walk section headers looking for .eh_frame (SHT_PROGBITS named ".eh_frame").
    // Modern toolchains always emit PT_GNU_EH_FRAME so this path is rarely taken.
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0 // GCOV_EXCL_START
        || ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(_Elf_Shdr) > map_size)
        goto done;

    const _Elf_Shdr* shdrs    = (const _Elf_Shdr*)((const char*)map + ehdr->e_shoff);
    const char*      shstrtab = NULL;
    if (ehdr->e_shstrndx < ehdr->e_shnum) {
        const _Elf_Shdr* s = &shdrs[ehdr->e_shstrndx];
        if (s->sh_offset + s->sh_size <= map_size)
            shstrtab = (const char*)map + s->sh_offset;
    }

    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        if (shdrs[i].sh_type != SHT_PROGBITS)
            continue;
        if (!shstrtab)
            continue;
        const char* name = shstrtab + shdrs[i].sh_name;
        if (strcmp(name, ".eh_frame") != 0)
            continue;
        if (shdrs[i].sh_offset + shdrs[i].sh_size > map_size)
            continue;

        entry->data     = (const uint8_t*)map + shdrs[i].sh_offset;
        entry->size     = (size_t)shdrs[i].sh_size;
        entry->sec_addr = (uintptr_t)((intptr_t)shdrs[i].sh_addr + slide);
        entry->slide    = slide;
        entry->map      = map;
        entry->map_size = map_size;

        log_d("cfi: loaded .eh_frame for %s (%zu bytes, slide=%" PRIdPTR ")", path, entry->size, slide);
        return entry;
    }

done:
    munmap(map, map_size);
    return NULL; // GCOV_EXCL_STOP
}

// ---------------------------------------------------------------------------
// FDE index builder — parse all FDE records once, sort by pc_begin
// ---------------------------------------------------------------------------

static int
_cfi_fde_cmp(const void* a, const void* b) {
    uintptr_t ka = ((const cfi_fde_t*)a)->pc_begin;
    uintptr_t kb = ((const cfi_fde_t*)b)->pc_begin;
    return (ka > kb) - (ka < kb);
}

// Parse the CIE referenced by an FDE to extract the FDE pointer encoding.
// Returns true on success, writing fde_ptr_enc and has_z.
static bool
_cfi_parse_cie(const uint8_t* cie_ptr, const uint8_t* data_end, uint8_t* fde_ptr_enc_out, bool* has_z_out) {
    const uint8_t* cp = cie_ptr;

    uint32_t cie_len32 = _read_u32(&cp);
    uint64_t cie_length;
    if (cie_len32 == 0xffffffff) {
        if (cp + 8 > data_end)
            return false;
        cie_length = _read_u64(&cp);
    } else {
        cie_length = cie_len32;
    }
    const uint8_t* cie_end = cp + cie_length;
    if (cie_end > data_end)
        return false;

    _read_u32(&cp); // CIE id (0)
    uint8_t version = *cp++;

    const char* aug = (const char*)cp;
    while (cp < cie_end && *cp)
        cp++;
    if (cp >= cie_end)
        return false;
    cp++;

    if (version >= 4) {
        cp++; // address_size
        cp++; // segment_selector_size
    }

    _read_uleb128(&cp, cie_end); // code_align
    _read_sleb128(&cp, cie_end); // data_align
    if (version == 1)
        cp++; // ra_col
    else
        _read_uleb128(&cp, cie_end);

    *fde_ptr_enc_out = DW_EH_PE_absptr;
    *has_z_out       = false;

    if (aug[0] == 'z') {
        *has_z_out = true;
        _read_uleb128(&cp, cie_end); // augmentation data length
        for (const char* a = aug + 1; *a && cp < cie_end; a++) {
            switch (*a) {
            case 'L':
                cp++;
                break;
            case 'R':
                *fde_ptr_enc_out = *cp++;
                break;
            case 'P': {
                uint8_t enc = *cp++;
                _read_encoded_ptr(&cp, cie_end, enc, 0);
                break;
            }
            case 'S':
                break;
            default:
                cp++;
            }
        }
    }
    return true;
}

// Build the sorted FDE index for a cache entry.  Called once after _cfi_load.
static void
_cfi_build_index(cfi_cache_entry_t* ce) {
    const uint8_t* p   = ce->data;
    const uint8_t* end = ce->data + ce->size;

    // First pass: count FDEs.
    size_t count = 0;
    {
        const uint8_t* q = p;
        while (q + 4 <= end) {
            uint32_t len32 = _read_u32(&q);
            uint64_t length;
            if (len32 == 0xffffffff) {
                if (q + 8 > end)
                    break;
                length = _read_u64(&q);
            } else {
                length = len32;
            }
            if (length == 0)
                break;
            const uint8_t* record_end = q + length;
            if (record_end > end)
                break;
            uint32_t cie_id = _read_u32(&q);
            if (cie_id != 0)
                count++;
            q = record_end;
        }
    }

    if (count == 0)
        return;

    cfi_fde_t* index = (cfi_fde_t*)malloc(count * sizeof(cfi_fde_t));
    if (!index)
        return;

    // Second pass: populate entries.
    size_t idx = 0;
    while (p + 4 <= end && idx < count) {
        uint32_t len32 = _read_u32(&p);
        uint64_t length;
        if (len32 == 0xffffffff) {
            if (p + 8 > end)
                break;
            length = _read_u64(&p);
        } else {
            length = len32;
        }
        if (length == 0)
            break;
        const uint8_t* record_end = p + length;
        if (record_end > end)
            break;

        uint32_t cie_id = _read_u32(&p);
        if (cie_id == 0) {
            p = record_end;
            continue;
        }

        const uint8_t* cie_ptr = (p - 4) - cie_id;
        if (cie_ptr < ce->data || cie_ptr + 4 > end) {
            p = record_end;
            continue;
        }

        uint8_t fde_ptr_enc;
        bool    has_z;
        if (!_cfi_parse_cie(cie_ptr, end, &fde_ptr_enc, &has_z)) {
            p = record_end;
            continue;
        }

        uintptr_t pc_offset_in_sec = (uintptr_t)(p - ce->data);
        uintptr_t pc_base          = ce->sec_addr + pc_offset_in_sec;

        uintptr_t pc_begin  = _read_encoded_ptr(&p, record_end, fde_ptr_enc, pc_base);
        uint8_t   range_enc = fde_ptr_enc & 0x0f;
        uintptr_t pc_range  = _read_encoded_ptr(&p, record_end, range_enc, 0);

        if (pc_begin == 0 || pc_range == 0) {
            p = record_end;
            continue;
        }

        // Skip augmentation data in FDE.
        if (has_z)
            _read_uleb128(&p, record_end);

        index[idx].pc_begin   = pc_begin;
        index[idx].pc_end     = pc_begin + pc_range;
        index[idx].cie_ptr    = cie_ptr;
        index[idx].fde_instrs = p;
        index[idx].fde_end    = record_end;
        idx++;

        p = record_end;
    }

    qsort(index, idx, sizeof(cfi_fde_t), _cfi_fde_cmp);

    ce->fde_index = index;
    ce->fde_count = idx;

    log_d("cfi: built FDE index with %zu entries", idx);
}

// ---------------------------------------------------------------------------
// FDE lookup and evaluation
// ---------------------------------------------------------------------------

// Parse the CIE fully (code_align, data_align, initial instructions) for eval.
static bool
_cfi_parse_cie_full(
    const uint8_t* cie_ptr, const uint8_t* data_end, uint64_t* code_align_out, int64_t* data_align_out,
    const uint8_t** initial_instr_out, const uint8_t** cie_end_out
) {
    const uint8_t* cp = cie_ptr;

    uint32_t cie_len32 = _read_u32(&cp);
    uint64_t cie_length;
    if (cie_len32 == 0xffffffff) {
        if (cp + 8 > data_end)
            return false;
        cie_length = _read_u64(&cp);
    } else {
        cie_length = cie_len32;
    }
    const uint8_t* cie_end = cp + cie_length;
    if (cie_end > data_end)
        return false;

    _read_u32(&cp); // CIE id (0)
    uint8_t version = *cp++;

    const char* aug = (const char*)cp;
    while (cp < cie_end && *cp)
        cp++;
    if (cp >= cie_end)
        return false;
    cp++;

    if (version >= 4) {
        cp++;
        cp++;
    }

    *code_align_out = _read_uleb128(&cp, cie_end);
    *data_align_out = _read_sleb128(&cp, cie_end);

    if (version == 1)
        cp++; // ra_col
    else
        _read_uleb128(&cp, cie_end);

    if (aug[0] == 'z') {
        _read_uleb128(&cp, cie_end);
        for (const char* a = aug + 1; *a && cp < cie_end; a++) {
            switch (*a) {
            case 'L':
                cp++;
                break;
            case 'R':
                cp++;
                break;
            case 'P': {
                uint8_t enc = *cp++;
                _read_encoded_ptr(&cp, cie_end, enc, 0);
                break;
            }
            case 'S':
                break;
            default:
                cp++;
            }
        }
    }

    *initial_instr_out = cp;
    *cie_end_out       = cie_end;
    return true;
}

// Binary-search the FDE index for the entry covering target_pc, then evaluate.
// Returns true on success and writes the CFI row to *out.
static bool
_cfi_find_and_eval(cfi_cache_entry_t* ce, uintptr_t target_pc, cfi_row_t* out) {
    if (target_pc == 0 || ce->fde_count == 0)
        return false;

    uintptr_t lookup_pc = target_pc - 1;

    // Binary search: find the last FDE with pc_begin <= lookup_pc.
    size_t lo = 0, hi = ce->fde_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ce->fde_index[mid].pc_begin <= lookup_pc)
            lo = mid;
        else
            hi = mid;
    }

    cfi_fde_t* fde = &ce->fde_index[lo];
    if (fde->pc_begin > lookup_pc || lookup_pc >= fde->pc_end)
        return false;

    // Parse the CIE for code_align, data_align, and initial instructions.
    uint64_t       code_align;
    int64_t        data_align;
    const uint8_t* cie_initial_instr;
    const uint8_t* cie_end;
    if (!_cfi_parse_cie_full(fde->cie_ptr, ce->data + ce->size, &code_align, &data_align, &cie_initial_instr, &cie_end))
        return false;

    // Initialise row from platform defaults.
    memset(out, 0, sizeof(*out));
#if defined(__x86_64__)
    out->cfa_reg = _CFI_SP_REG;
    out->cfa_off = 8;
#elif defined(__aarch64__)
    out->cfa_reg = _CFI_SP_REG;
    out->cfa_off = 0;
#endif
    for (int i = 0; i < _CFI_MAX_REGS; i++)
        out->regs[i].kind = REG_UNDEF;

    // Apply CIE initial instructions.
    if (!_cfi_eval(cie_initial_instr, cie_end, 0, UINTPTR_MAX, code_align, data_align, out, NULL))
        return false;
    cfi_row_t cie_row = *out;

    // Apply FDE instructions up to lookup_pc.
    if (!_cfi_eval(fde->fde_instrs, fde->fde_end, fde->pc_begin, lookup_pc, code_align, data_align, out, &cie_row))
        return false;

    if (out->regs[_CFI_RA_REG].kind == REG_UNDEF) {
        out->regs[_CFI_RA_REG].kind = REG_CFA_OFFSET;
#if defined(__x86_64__)
        out->regs[_CFI_RA_REG].offset = -8;
#endif
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// cfi_step: given the current pc/sp/fp of a ptrace-stopped thread, use
// .eh_frame CFI to find the caller's pc and sp.
//
// Arguments:
//   pid                       — the target process
//   maps_tree                 — vm_range_tree built from /proc/<pid>/maps
//   pc, sp, fp                — current register values (IN), updated to caller's (OUT)
//   stack_buf, stack_buf_base — optional prefetched stack page (NULL to disable);
//                               CFA reads that fall within this window skip the
//                               process_vm_readv syscall entirely, turning the
//                               per-step cost from ~1 µs to a cache-hot memcpy.
//
// Returns true if a step was successfully taken.
static bool
cfi_step(
    pid_t pid, vm_range_tree_t* maps_tree, uintptr_t* pc, uintptr_t* sp, uintptr_t* fp, const uint8_t* stack_buf,
    uintptr_t stack_buf_base, size_t stack_buf_size
) {
    vm_range_t* range = vm_range_tree__find(maps_tree, *pc);
    if (!range)
        return false;

    // range->lo is the load base for this binary: _py_proc__get_vm_maps creates
    // each range starting at the first (lowest) mapping address for its pathname,
    // which is the same value that base_table stores.  Using it directly avoids
    // a string hash + hash-table lookup on every unwind step.
    uintptr_t load_base = range->lo;
    if (!load_base)
        return false;

    cfi_cache_entry_t* ce = _cfi_load(range->name, load_base, pid);
    if (!ce)
        return false;

    // Build the sorted FDE index on first use.
    if (ce->fde_index == NULL && ce->fde_count == 0)
        _cfi_build_index(ce);

    cfi_row_t row;
    if (!_cfi_find_and_eval(ce, *pc, &row))
        return false;

    // Compute CFA.
    uintptr_t reg_val;
    if (row.cfa_reg == _CFI_SP_REG) {
        reg_val = *sp;
    } else if (row.cfa_reg == _CFI_FP_REG) {
        reg_val = *fp;
    } else {
        return false; // uncommon; give up
    }
    uintptr_t cfa = (uintptr_t)((intptr_t)reg_val + row.cfa_off);

    // Recover return address from *(CFA + ra_offset).
    if (row.regs[_CFI_RA_REG].kind != REG_CFA_OFFSET)
        return false;

    uintptr_t ra_addr = (uintptr_t)((intptr_t)cfa + row.regs[_CFI_RA_REG].offset);
    uintptr_t new_pc  = 0;
    uintptr_t new_fp  = 0;

    bool      fp_saved = row.regs[_CFI_FP_REG].kind == REG_CFA_OFFSET;
    uintptr_t fp_addr  = fp_saved ? (uintptr_t)((intptr_t)cfa + row.regs[_CFI_FP_REG].offset) : 0;

// Check whether a remote address falls entirely within the prefetched buffer.
#define _IN_SBUF(addr)                                                                                               \
    (stack_buf != NULL && (addr) >= stack_buf_base && (addr) + sizeof(uintptr_t) <= stack_buf_base + stack_buf_size)
#define _SBUF_READ(addr, dst) memcpy((dst), stack_buf + ((addr) - stack_buf_base), sizeof(uintptr_t))

    bool ra_in_buf = _IN_SBUF(ra_addr);
    bool fp_in_buf = fp_saved && _IN_SBUF(fp_addr);

    if (ra_in_buf) {
        // Fast path: RA (and possibly FP) served from the prefetched stack page —
        // no syscall needed for the common case of a shallow native stack.
        _SBUF_READ(ra_addr, &new_pc);
        if (fp_saved) {
            if (fp_in_buf)
                _SBUF_READ(fp_addr, &new_fp);
            else {
                struct iovec l = {.iov_base = &new_fp, .iov_len = sizeof(new_fp)};
                struct iovec r = {.iov_base = (void*)fp_addr, .iov_len = sizeof(new_fp)};
                process_vm_readv(pid, &l, 1, &r, 1, 0); // best effort; FP failure is tolerable
            }
        }
    } else if (fp_saved) {
        // Fallback: batch RA + FP into a single syscall (original behaviour).
        struct iovec local[2] = {
            {.iov_base = &new_pc, .iov_len = sizeof(new_pc)},
            {.iov_base = &new_fp, .iov_len = sizeof(new_fp)}
        };
        struct iovec remote[2] = {
            {.iov_base = (void*)ra_addr, .iov_len = sizeof(new_pc)},
            {.iov_base = (void*)fp_addr, .iov_len = sizeof(new_fp)}
        };
        ssize_t n = process_vm_readv(pid, local, 2, remote, 2, 0);
        if (n < (ssize_t)sizeof(new_pc))
            return false;
    } else {
        // Fallback: RA only.
        struct iovec local  = {.iov_base = &new_pc, .iov_len = sizeof(new_pc)};
        struct iovec remote = {.iov_base = (void*)ra_addr, .iov_len = sizeof(new_pc)};
        if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != (ssize_t)sizeof(new_pc))
            return false;
    }

#undef _IN_SBUF
#undef _SBUF_READ

    *pc = new_pc;
    *sp = cfa;
    *fp = new_fp;
    return true;
}

// ---------------------------------------------------------------------------
// Cleanup (call once at process exit to free cached maps)
// ---------------------------------------------------------------------------

static void
cfi_cache_destroy(void) {
    for (unsigned i = 0; i < _CFI_CACHE_BUCKETS; i++) {
        cfi_cache_entry_t* e = _cfi_cache[i];
        while (e) {
            cfi_cache_entry_t* next = e->next;
            if (e->map)
                munmap(e->map, e->map_size);
            free(e->fde_index);
            free(e);
            e = next;
        }
        _cfi_cache[i] = NULL;
    }
}

#endif /* PL_LINUX && !AUSTINP && (x86_64 || aarch64) */
