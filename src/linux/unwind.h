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

#if defined(NATIVE) && defined(PL_LINUX) && !defined(AUSTINP)

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

// Per-binary .eh_frame cache entry.
typedef struct _cfi_cache_entry {
    uint64_t                 key;      // string hash of the binary path
    const uint8_t*           data;     // pointer into the mmap'd file
    size_t                   size;     // byte size of .eh_frame
    uintptr_t                sec_addr; // runtime VA of the section start
    intptr_t                 slide;    // ASLR slide applied to FDE pc values
    void*                    map;      // mmap base (kept alive)
    size_t                   map_size;
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
    int64_t data_align, cfi_row_t* row
) {
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
            if (reg < _CFI_MAX_REGS)
                row->regs[reg].kind = REG_UNDEF;
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
        case DW_CFA_def_cfa_sf: {
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
        }

        case DW_CFA_offset_extended: {
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
        }

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
        case DW_CFA_restore_extended: {
            uint64_t reg = _read_uleb128(&p, end);
            if (reg < _CFI_MAX_REGS)
                row->regs[reg].kind = REG_UNDEF;
            break;
        }
        case DW_CFA_register: {
            // reg1 = reg2 — uncommon, skip both operands
            _read_uleb128(&p, end);
            _read_uleb128(&p, end);
            break;
        }

        case DW_CFA_advance_loc1:
            if (p + 1 > end)
                return false;
            row_pc += *p++ * code_align;
            break;
        case DW_CFA_advance_loc2:
            if (p + 2 > end)
                return false;
            row_pc += _read_u16(&p) * code_align;
            break;
        case DW_CFA_advance_loc3:
            if (p + 4 > end)
                return false;
            row_pc += _read_u32(&p) * code_align;
            break;
        case DW_CFA_set_loc: {
            // New row_pc is an encoded address; we use absptr for simplicity.
            uintptr_t loc;
            if (p + sizeof(uintptr_t) > end)
                return false;
            memcpy(&loc, p, sizeof(uintptr_t));
            p      += sizeof(uintptr_t);
            row_pc  = loc;
            break;
        }

        // These require a state stack; just skip the operands.
        case DW_CFA_remember_state:
        case DW_CFA_restore_state:
            break;

        // Skip size argument for GNU_args_size.
        case DW_CFA_GNU_args_size:
            _read_uleb128(&p, end);
            break;

        // DWARF expressions: skip by reading block length then skipping.
        case DW_CFA_def_cfa_expression:
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
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// .eh_frame loader — locates and caches the section for a binary
// ---------------------------------------------------------------------------

static cfi_cache_entry_t*
_cfi_load(const char* path, uintptr_t load_base) {
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

    cu_fd fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr))
        return NULL;

    void* map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        return NULL;

    size_t map_size = (size_t)st.st_size;

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

    // Walk section headers looking for .eh_frame (SHT_PROGBITS named ".eh_frame").
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0
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
    return NULL;
}

// ---------------------------------------------------------------------------
// FDE scanner
// ---------------------------------------------------------------------------

// Evaluate the FDE that covers `target_pc` to obtain the CFI row.
// Returns true on success and writes the row to *out.
static bool
_cfi_find_and_eval(cfi_cache_entry_t* ce, uintptr_t target_pc, cfi_row_t* out) {
    const uint8_t* p   = ce->data;
    const uint8_t* end = ce->data + ce->size;

    while (p + 4 <= end) {
        // Read length field (4- or 12-byte extended form).
        uint32_t len32 = _read_u32(&p);
        uint64_t length;
        if (len32 == 0xffffffff) {
            if (p + 8 > end)
                return false;
            length = _read_u64(&p);
        } else {
            length = len32;
        }
        if (length == 0)
            break; // terminator

        const uint8_t* record_end = p + length;
        if (record_end > end)
            break;

        // CIE/FDE discriminator: CIE_id == 0 for CIE, else offset to CIE.
        uint32_t cie_id = _read_u32(&p);

        if (cie_id == 0) {
            // ----- CIE ----- skip it; we re-read when we find an FDE.
            p = record_end;
            continue;
        }

        // ----- FDE -----
        // The CIE pointer is a relative offset back from the current position.
        const uint8_t* cie_ptr = (p - 4) - cie_id;
        if (cie_ptr < ce->data || cie_ptr + 4 > end) {
            p = record_end;
            continue;
        }

        // Re-parse the CIE to get code_align, data_align, ra_col, and the
        // augmentation string / initial instructions.
        const uint8_t* cp = cie_ptr;

        uint32_t cie_len32 = _read_u32(&cp);
        uint64_t cie_length;
        if (cie_len32 == 0xffffffff) {
            if (cp + 8 > end) {
                p = record_end;
                continue;
            }
            cie_length = _read_u64(&cp);
        } else {
            cie_length = cie_len32;
        }
        const uint8_t* cie_end = cp + cie_length;

        /* uint32_t cie_id_check = */ _read_u32(&cp); // should be 0
        uint8_t version = *cp++;

        // Augmentation string (NUL-terminated).
        const char* aug = (const char*)cp;
        while (cp < cie_end && *cp)
            cp++;
        if (cp >= cie_end) {
            p = record_end;
            continue;
        }
        cp++; // skip NUL

        // EH data pointer size (only in version 4+).
        if (version >= 4) {
            cp++; // address_size
            cp++; // segment_selector_size
        }

        uint64_t code_align = _read_uleb128(&cp, cie_end);
        int64_t  data_align = _read_sleb128(&cp, cie_end);

        // Return address register column.
        uint64_t ra_col;
        if (version == 1)
            ra_col = *cp++;
        else
            ra_col = _read_uleb128(&cp, cie_end);

        // Parse 'z' augmentation to get FDE pointer encoding.
        uint8_t fde_ptr_enc = DW_EH_PE_absptr;
        bool    has_z       = false;
        if (aug[0] == 'z') {
            has_z = true;
            /* uint64_t aug_len = */ _read_uleb128(&cp, cie_end);
            for (const char* a = aug + 1; *a && cp < cie_end; a++) {
                switch (*a) {
                case 'L':
                    cp++;
                    break; // LSDA encoding
                case 'R':
                    fde_ptr_enc = *cp++;
                    break;  // FDE pointer encoding
                case 'P': { // personality
                    uint8_t enc = *cp++;
                    _read_encoded_ptr(&cp, cie_end, enc, 0);
                    break;
                }
                case 'S':
                    break; // signal frame flag
                default:
                    cp++;
                }
            }
        }

        const uint8_t* cie_initial_instr = cp;

        // Now parse the FDE header (pc_begin, pc_range).
        uintptr_t pc_offset_in_sec = (uintptr_t)(p - ce->data);
        uintptr_t pc_base          = ce->sec_addr + pc_offset_in_sec;

        uintptr_t pc_begin = _read_encoded_ptr(&p, record_end, fde_ptr_enc, pc_base);
        uintptr_t pc_range;
        uint8_t   range_enc = fde_ptr_enc & 0x0f; // same type, no application
        pc_range            = _read_encoded_ptr(&p, record_end, range_enc, 0);

        if (pc_begin == 0 || pc_begin > target_pc || target_pc >= pc_begin + pc_range) {
            p = record_end;
            continue;
        }

        // Found the FDE for target_pc.
        // Initialise row from the CIE initial instructions.
        memset(out, 0, sizeof(*out));
#if defined(__x86_64__)
        out->cfa_reg = _CFI_SP_REG;
        out->cfa_off = 8; // at function entry, CFA = RSP + 8
#elif defined(__aarch64__)
        out->cfa_reg = _CFI_SP_REG;
        out->cfa_off = 0;
#endif
        for (int i = 0; i < _CFI_MAX_REGS; i++)
            out->regs[i].kind = REG_UNDEF;

        // Apply CIE initial instructions.
        if (!_cfi_eval(cie_initial_instr, cie_end, pc_begin, target_pc, code_align, data_align, out))
            return false;

        // Skip augmentation data in FDE (zR produces a length-prefixed block).
        if (has_z)
            _read_uleb128(&p, record_end); // augmentation data length

        // Apply FDE instructions up to target_pc.
        if (!_cfi_eval(p, record_end, pc_begin, target_pc, code_align, data_align, out))
            return false;

        out->regs[_CFI_RA_REG].kind = REG_CFA_OFFSET;
        // If the RA rule was not set by instructions keep the x86 default (-8)
        if (out->regs[_CFI_RA_REG].offset == 0 && out->regs[_CFI_RA_REG].kind == REG_CFA_OFFSET) {
#if defined(__x86_64__)
            out->regs[_CFI_RA_REG].offset = -8;
#endif
        }
        (void)ra_col;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API: cfi_step
// ---------------------------------------------------------------------------

// cfi_step: given the current pc/sp/fp of a ptrace-stopped thread, use
// .eh_frame CFI to find the caller's pc and sp.
//
// Arguments:
//   pid        — the target process
//   maps_tree  — vm_range_tree built from /proc/<pid>/maps
//   base_table — hash table mapping path hash -> runtime load_base
//   pc, sp, fp — current register values (IN), updated to caller's (OUT)
//
// Returns true if a step was successfully taken.
static bool
cfi_step(pid_t pid, vm_range_tree_t* maps_tree, hash_table_t* base_table, uintptr_t* pc, uintptr_t* sp, uintptr_t* fp) {
    vm_range_t* range = vm_range_tree__find(maps_tree, *pc);
    if (!range)
        return false;

    uintptr_t load_base = (uintptr_t)hash_table__get(base_table, string__hash(range->name));
    if (!load_base)
        return false;

    cfi_cache_entry_t* ce = _cfi_load(range->name, load_base);
    if (!ce)
        return false;

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

    uintptr_t    ra_addr = (uintptr_t)((intptr_t)cfa + row.regs[_CFI_RA_REG].offset);
    uintptr_t    new_pc  = 0;
    struct iovec local   = {.iov_base = &new_pc, .iov_len = sizeof(new_pc)};
    struct iovec remote  = {.iov_base = (void*)ra_addr, .iov_len = sizeof(new_pc)};
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != (ssize_t)sizeof(new_pc))
        return false;

    if (new_pc == 0)
        return false;

    // Optionally recover saved FP (best-effort; not fatal if absent).
    uintptr_t new_fp = 0;
    if (row.regs[_CFI_FP_REG].kind == REG_CFA_OFFSET) {
        uintptr_t    fp_addr = (uintptr_t)((intptr_t)cfa + row.regs[_CFI_FP_REG].offset);
        struct iovec lf      = {.iov_base = &new_fp, .iov_len = sizeof(new_fp)};
        struct iovec rf      = {.iov_base = (void*)fp_addr, .iov_len = sizeof(new_fp)};
        process_vm_readv(pid, &lf, 1, &rf, 1, 0); // ignore failure
    }

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
            free(e);
            e = next;
        }
        _cfi_cache[i] = NULL;
    }
}

#endif /* NATIVE && PL_LINUX && !AUSTINP */
