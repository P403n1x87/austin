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

// Userspace PE .pdata/.xdata stack unwinder for austin on Windows.
//
// This is the Windows analogue of src/linux/unwind.h.  Instead of parsing
// DWARF .eh_frame, we parse the PE exception directory (.pdata) and unwind
// data (.xdata) to walk the native call stack without any per-frame OS API
// calls (no StackWalk64, no RtlVirtualUnwind).
//
// Per-module data is cached on first use.  The cache is keyed by image base
// address and stores:
//   - the sorted RUNTIME_FUNCTION array (.pdata) for binary search
//   - the raw unwind data section (.xdata) for opcode interpretation
//
// Supports x64 unwind codes.

#pragma once

#include <windows.h>

#include "../logging.h"

// ---------------------------------------------------------------------------
// x64 GP register indices (PE unwind encoding)
// ---------------------------------------------------------------------------

#if defined(_M_X64)
#define REG_RAX      0
#define REG_RCX      1
#define REG_RDX      2
#define REG_RBX      3
#define REG_RSP      4
#define REG_RBP      5
#define REG_RSI      6
#define REG_RDI      7
#define REG_R8       8
#define REG_R9       9
#define REG_R10      10
#define REG_R11      11
#define REG_R12      12
#define REG_R13      13
#define REG_R14      14
#define REG_R15      15
#define GP_REG_COUNT 16
#endif

// ---------------------------------------------------------------------------
// PE UNWIND_INFO structures
// ---------------------------------------------------------------------------

#ifndef UNW_FLAG_CHAININFO
#define UNW_FLAG_CHAININFO 0x04
#endif

// ---- x64 unwind structures ------------------------------------------------

typedef struct {
    uint8_t Version : 3;
    uint8_t Flags : 5;
    uint8_t SizeOfProlog;
    uint8_t CountOfCodes;
    uint8_t FrameRegister : 4;
    uint8_t FrameOffset : 4;
    // UNWIND_CODE UnwindCode[...] follows
} pe_unwind_info_t;

typedef union {
    struct {
        uint8_t CodeOffset;
        uint8_t UnwindOp : 4;
        uint8_t OpInfo : 4;
    };
    uint16_t FrameOffset;
} pe_unwind_code_t;

// x64 unwind operation codes.
#define UWOP_PUSH_NONVOL     0
#define UWOP_ALLOC_LARGE     1
#define UWOP_ALLOC_SMALL     2
#define UWOP_SET_FPREG       3
#define UWOP_SAVE_NONVOL     4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_SAVE_XMM128     8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME  10

// ---------------------------------------------------------------------------
// .pdata cache
// ---------------------------------------------------------------------------

typedef struct _pdata_cache_entry {
    uintptr_t                  image_base;
    RUNTIME_FUNCTION*          funcs; // local copy of the .pdata array
    DWORD                      count; // number of RUNTIME_FUNCTION entries
    uint8_t*                   xdata; // local copy of unwind data section(s)
    size_t                     xdata_size;
    uintptr_t                  xdata_rva; // RVA of the xdata section start
    // Absolute [begin, end) address range covering the largest function (==
    // _PyEval_EvalFrameDefault) and any absorbed sub-entries/cold-blocks.
    uintptr_t                  largest_func_begin;
    uintptr_t                  largest_func_end;
    // BeginAddress RVA of the main body of the largest function.  This is
    // the canonical identifier used to recognise cold-block (CHAININFO)
    // sub-entries via _pdata_logical_root comparisons.
    DWORD                      main_func_rva;
    struct _pdata_cache_entry* next;
} pdata_cache_entry_t;

#define _PDATA_CACHE_BUCKETS 64
static pdata_cache_entry_t* _pdata_cache[_PDATA_CACHE_BUCKETS] = {0};

static inline pdata_cache_entry_t*
_pdata_cache_lookup(uintptr_t image_base) {
    size_t               bucket = (image_base >> 12) % _PDATA_CACHE_BUCKETS;
    pdata_cache_entry_t* e      = _pdata_cache[bucket];
    while (e) {
        if (e->image_base == image_base)
            return e;
        e = e->next;
    }
    return NULL;
}

// Get a pointer into the cached xdata buffer for a given RVA.
// Returns NULL if the RVA is outside the cached region.
static inline void*
_pdata_get_xdata(pdata_cache_entry_t* ce, DWORD rva, size_t min_size) {
    if (ce->xdata == NULL || rva < ce->xdata_rva || (rva - ce->xdata_rva) + min_size > ce->xdata_size)
        return NULL;
    return ce->xdata + (rva - ce->xdata_rva);
}

// Get a pointer to the UNWIND_INFO for a RUNTIME_FUNCTION from the cached
// xdata buffer.  Returns NULL if the data is outside the cached region.
static inline pe_unwind_info_t*
_pdata_get_unwind_info(pdata_cache_entry_t* ce, RUNTIME_FUNCTION* rf) {
    return (pe_unwind_info_t*)_pdata_get_xdata(ce, rf->UnwindData, sizeof(pe_unwind_info_t));
}

// Follow UNW_FLAG_CHAININFO links and return the logical root RUNTIME_FUNCTION.
// A chained RF delegates its unwind to a parent RF; the root has no chain.
// Returns rf itself if unchained or if the chain cannot be resolved.
static inline RUNTIME_FUNCTION*
_pdata_logical_root(pdata_cache_entry_t* ce, RUNTIME_FUNCTION* rf) {
    int depth = 8; // guard against corrupt data
    while (rf && depth-- > 0) {
        pe_unwind_info_t* ui = _pdata_get_unwind_info(ce, rf);
        if (!ui || !(ui->Flags & UNW_FLAG_CHAININFO))
            break;
        // The chained RUNTIME_FUNCTION follows the unwind codes (even-aligned).
        DWORD             code_words = ((DWORD)ui->CountOfCodes + 1u) & ~1u;
        DWORD             chain_off  = rf->UnwindData + sizeof(pe_unwind_info_t) + code_words * 2;
        RUNTIME_FUNCTION* parent     = (RUNTIME_FUNCTION*)_pdata_get_xdata(ce, chain_off, sizeof(RUNTIME_FUNCTION));
        if (!parent)
            break;
        rf = parent;
    }
    return rf;
}

// Read the PE exception directory (.pdata) and unwind data section from a
// remote module and cache them locally.
static inline pdata_cache_entry_t*
_pdata_cache_load(HANDLE hProcess, uintptr_t image_base) {
    IMAGE_DOS_HEADER dos_hdr;
    SIZE_T           n = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)image_base, &dos_hdr, sizeof(dos_hdr), &n) || n != sizeof(dos_hdr))
        return NULL;
    if (dos_hdr.e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;

    IMAGE_NT_HEADERS nt_hdr;
    uintptr_t        pe_addr = image_base + dos_hdr.e_lfanew;
    if (!ReadProcessMemory(hProcess, (LPCVOID)pe_addr, &nt_hdr, sizeof(nt_hdr), &n) || n != sizeof(nt_hdr))
        return NULL;
    if (nt_hdr.Signature != IMAGE_NT_SIGNATURE)
        return NULL;

    IMAGE_DATA_DIRECTORY* exc_dir = &nt_hdr.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exc_dir->VirtualAddress == 0 || exc_dir->Size == 0)
        return NULL;

    DWORD count = exc_dir->Size / sizeof(RUNTIME_FUNCTION);
    if (count == 0)
        return NULL;

    RUNTIME_FUNCTION* funcs = (RUNTIME_FUNCTION*)malloc(exc_dir->Size);
    if (!funcs)
        return NULL;

    uintptr_t pdata_addr = image_base + exc_dir->VirtualAddress;
    if (!ReadProcessMemory(hProcess, (LPCVOID)pdata_addr, funcs, exc_dir->Size, &n) || n != exc_dir->Size) {
        free(funcs);
        return NULL;
    }

    // Read the section containing unwind data.  Find the section that holds
    // the UnwindData of the first RUNTIME_FUNCTION.
    uint8_t*  xdata      = NULL;
    size_t    xdata_size = 0;
    uintptr_t xdata_rva  = 0;

    DWORD     num_sections = nt_hdr.FileHeader.NumberOfSections;
    uintptr_t sec_hdr_addr
        = pe_addr + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt_hdr.FileHeader.SizeOfOptionalHeader;

    if (num_sections > 0 && num_sections < 96 && count > 0) {
        IMAGE_SECTION_HEADER* sections = (IMAGE_SECTION_HEADER*)malloc(num_sections * sizeof(IMAGE_SECTION_HEADER));
        if (sections) {
            if (ReadProcessMemory(
                    hProcess, (LPCVOID)sec_hdr_addr, sections, num_sections * sizeof(IMAGE_SECTION_HEADER), &n
                )
                && n == num_sections * sizeof(IMAGE_SECTION_HEADER)) {
                // Find the range of all sections that contain unwind data by
                // checking the first and last RUNTIME_FUNCTION entries (the
                // array is sorted by BeginAddress, and UnwindData RVAs
                // generally span the same range).
                DWORD rva_first = funcs[0].UnwindData;
                DWORD rva_last  = funcs[count - 1].UnwindData;
                DWORD range_lo  = (DWORD)-1;
                DWORD range_hi  = 0;

                for (DWORD i = 0; i < num_sections; i++) {
                    DWORD sec_start      = sections[i].VirtualAddress;
                    DWORD sec_end        = sec_start + sections[i].Misc.VirtualSize;
                    bool  contains_first = (rva_first >= sec_start && rva_first < sec_end);
                    bool  contains_last  = (rva_last >= sec_start && rva_last < sec_end);
                    if (contains_first || contains_last) {
                        if (sec_start < range_lo)
                            range_lo = sec_start;
                        if (sec_end > range_hi)
                            range_hi = sec_end;
                    }
                }

                if (range_lo < range_hi) {
                    xdata_rva  = range_lo;
                    xdata_size = range_hi - range_lo;
                    xdata      = (uint8_t*)malloc(xdata_size);
                    if (xdata) {
                        if (!ReadProcessMemory(hProcess, (LPCVOID)(image_base + range_lo), xdata, xdata_size, &n)
                            || n != xdata_size) {
                            free(xdata);
                            xdata      = NULL;
                            xdata_size = 0;
                            xdata_rva  = 0;
                        }
                    }
                }
            }
            free(sections);
        }
    }

    pdata_cache_entry_t* entry = (pdata_cache_entry_t*)calloc(1, sizeof(pdata_cache_entry_t));
    if (!entry) {
        free(funcs);
        free(xdata);
        return NULL;
    }
    entry->image_base = image_base;
    entry->funcs      = funcs;
    entry->count      = count;
    entry->xdata      = xdata;
    entry->xdata_size = xdata_size;
    entry->xdata_rva  = xdata_rva;

    // Find the largest RUNTIME_FUNCTION in this module.  _PyEval_EvalFrameDefault
    // is always the largest function in any CPython build by a large margin, so
    // the biggest pdata entry reliably identifies it even without PDB symbols.
    //
    // The compiler can split a large function into non-contiguous sub-sections.
    // These appear as:
    //   (a) UNW_FLAG_CHAININFO entries that reference the main body RF, or
    //   (b) independent sub-entries placed at distant addresses (cold blocks).
    //
    // Strategy: find the main body (largest RF), then do two passes:
    //   1. Scan all RFs for UNW_FLAG_CHAININFO links back to the main body.
    //   2. Absorb adjacent entries within a 256-byte gap (handles case (b) when
    //      the compiler does NOT emit a CHAININFO for cold blocks).
    DWORD largest_size        = 0;
    DWORD largest_idx         = 0;
    entry->largest_func_begin = 0;
    entry->largest_func_end   = 0;
    entry->main_func_rva      = 0;
    for (DWORD i = 0; i < count; i++) {
        DWORD sz = funcs[i].EndAddress - funcs[i].BeginAddress;
        if (sz > largest_size) {
            largest_size              = sz;
            largest_idx               = i;
            entry->largest_func_begin = image_base + funcs[i].BeginAddress;
            entry->largest_func_end   = image_base + funcs[i].EndAddress;
        }
    }
    if (entry->largest_func_begin != 0) {
        // Save the main body's RVA before Pass 1 may expand largest_func_begin.
        entry->main_func_rva = funcs[largest_idx].BeginAddress;

        RUNTIME_FUNCTION* main_rf    = &funcs[largest_idx];
        DWORD             main_begin = main_rf->BeginAddress;

        // Pass 1: follow UNW_FLAG_CHAININFO links — absorb any RF whose
        // logical root is the main body RF (handles both adjacent sub-entries
        // and cold blocks at completely different addresses).
        // NOTE: _pdata_logical_root returns a pointer into the xdata buffer
        // when a CHAININFO chain is followed, while main_rf points into the
        // funcs buffer.  Pointer identity is therefore UNRELIABLE; compare
        // BeginAddress fields instead.
        for (DWORD i = 0; i < count; i++) {
            if (i == largest_idx)
                continue;
            pe_unwind_info_t* ui = _pdata_get_unwind_info(entry, &funcs[i]);
            if (!ui || !(ui->Flags & UNW_FLAG_CHAININFO))
                continue;
            RUNTIME_FUNCTION* root = _pdata_logical_root(entry, &funcs[i]);
            if (!root || root->BeginAddress != main_begin)
                continue;
            uintptr_t rf_begin = image_base + funcs[i].BeginAddress;
            uintptr_t rf_end   = image_base + funcs[i].EndAddress;
            log_d("win: eval frame: absorb chained RF [%" PRIxPTR ", %" PRIxPTR ") -> main body", rf_begin, rf_end);
            if (rf_begin < entry->largest_func_begin)
                entry->largest_func_begin = rf_begin;
            if (rf_end > entry->largest_func_end)
                entry->largest_func_end = rf_end;
        }

        // Pass 2: absorb immediately adjacent non-chained entries (gap <= 256
        // bytes from the ORIGINAL boundary).  Snapshot begin/end first so the
        // gap check uses fixed reference points — updating them inside the loop
        // would let the boundary cascade and sweep the entire DLL (every pair
        // of contiguous functions has gap == 0, which is always <= 256).
        const DWORD gap         = 256;
        uintptr_t   fixed_begin = entry->largest_func_begin;
        uintptr_t   fixed_end   = entry->largest_func_end;
        for (int j = (int)largest_idx - 1; j >= 0; j--) {
            uintptr_t rf_end = image_base + funcs[j].EndAddress;
            if (rf_end > fixed_begin || fixed_begin - rf_end > gap)
                break;
            entry->largest_func_begin = image_base + funcs[j].BeginAddress;
        }
        for (DWORD j = largest_idx + 1; j < count; j++) {
            uintptr_t rf_begin = image_base + funcs[j].BeginAddress;
            if (rf_begin < fixed_end || rf_begin - fixed_end > gap)
                break;
            entry->largest_func_end = image_base + funcs[j].EndAddress;
        }
    }
    log_d(
        "win: eval frame region in module at %" PRIxPTR ": [%" PRIxPTR ", %" PRIxPTR ") largest_size=%lu", image_base,
        entry->largest_func_begin, entry->largest_func_end, (unsigned long)largest_size
    );

    size_t bucket        = (image_base >> 12) % _PDATA_CACHE_BUCKETS;
    entry->next          = _pdata_cache[bucket];
    _pdata_cache[bucket] = entry;

    log_d("win: cached %lu .pdata entries for module at %" PRIxPTR, (unsigned long)count, image_base);
    return entry;
}

// Binary search for the RUNTIME_FUNCTION covering a given RVA.
static inline RUNTIME_FUNCTION*
_pdata_find(pdata_cache_entry_t* ce, DWORD rva) {
    DWORD lo = 0, hi = ce->count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (ce->funcs[mid].EndAddress <= rva)
            lo = mid + 1;
        else if (ce->funcs[mid].BeginAddress > rva)
            hi = mid;
        else
            return &ce->funcs[mid];
    }
    return NULL;
}

static void
_pdata_cache_destroy(void) {
    for (int i = 0; i < _PDATA_CACHE_BUCKETS; i++) {
        pdata_cache_entry_t* e = _pdata_cache[i];
        while (e) {
            pdata_cache_entry_t* next = e->next;
            free(e->funcs);
            free(e->xdata);
            free(e);
            e = next;
        }
        _pdata_cache[i] = NULL;
    }
}

// ---------------------------------------------------------------------------
// Userspace PE unwind — x64
// ---------------------------------------------------------------------------
//
// Interpret UNWIND_CODE opcodes from a cached UNWIND_INFO to recover CFA and
// return address.  On x64, the return address was pushed by CALL onto [RSP]
// before the prologue ran.  The prologue may push registers and allocate stack
// space.  The unwind codes describe how to reverse the prologue.
//
// We only need to recover: RSP (to find CFA), RIP (return address), and
// optionally RBP (for downstream fp-walk).

#if defined(_M_X64)

// Try to detect and simulate a function epilog at the current PC.
// Returns true if an epilog was detected and simulated (registers updated).
// Returns false if not in an epilog (caller should use unwind codes).
static inline bool
_pe_try_epilog(
    HANDLE hProcess, uintptr_t pc, uintptr_t func_end, uint8_t frame_reg, uintptr_t* rip, uintptr_t gp[GP_REG_COUNT]
) {
    // Read up to 128 bytes of instructions from pc to func_end.
    size_t  remaining = (size_t)(func_end - pc);
    uint8_t ibuf[128];
    log_d("win: epilog? pc=%" PRIxPTR " func_end=%" PRIxPTR " remaining=%zu", pc, func_end, remaining);
    if (remaining == 0 || remaining > sizeof(ibuf)) {
        log_d("win: epilog skip: remaining=%zu out of range", remaining);
        return false;
    }

    SIZE_T n = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)pc, ibuf, remaining, &n) || n != remaining)
        return false;

    const uint8_t* p   = ibuf;
    const uint8_t* end = ibuf + remaining;
    uintptr_t      sp  = gp[REG_RSP];
    uintptr_t      bp  = gp[REG_RBP];

    // First instruction may be: add rsp,imm8/imm32  OR  lea rsp,[fp+disp8/disp32]
    if (p < end) {
        uint8_t        rex = 0;
        const uint8_t* q   = p;
        if ((*q & 0xf0) == 0x40) {
            rex = *q & 0x0f;
            q++;
        }
        if (rex & 0x08) {
            // add rsp, imm8:  48 83 c4 xx
            if (q + 2 < end && q[0] == 0x83 && q[1] == 0xc4) {
                sp += (uintptr_t)q[2];
                p   = q + 3;
            }
            // add rsp, imm32: 48 81 c4 xx xx xx xx
            else if (q + 5 < end && q[0] == 0x81 && q[1] == 0xc4) {
                uint32_t imm;
                memcpy(&imm, q + 2, 4);
                sp += imm;
                p   = q + 6;
            }
            // lea rsp, [fp+disp8]:  48 8d 6x xx  (where x encodes the frame register)
            else if (q + 2 < end && q[0] == 0x8d && frame_reg != 0) {
                uint8_t modrm   = q[1];
                uint8_t mod_    = modrm >> 6;
                uint8_t reg_    = (modrm >> 3) & 7;
                uint8_t rm      = modrm & 7;
                uint8_t src_reg = rm | ((rex & 0x01) << 3);
                uint8_t dst_reg = reg_ | ((rex & 0x04) << 1);
                if (dst_reg == 4 /* RSP */ && src_reg == frame_reg) {
                    if (mod_ == 0x01 && q + 3 <= end) {
                        int8_t disp8 = (int8_t)q[2];
                        sp           = gp[frame_reg] + disp8;
                        p            = q + 3;
                    } else if (mod_ == 0x02 && q + 6 <= end) {
                        int32_t disp32;
                        memcpy(&disp32, q + 2, 4);
                        sp = gp[frame_reg] + disp32;
                        p  = q + 6;
                    }
                }
            }
        }
    }

    // Then zero or more pop r64 instructions.
    while (p < end) {
        uint8_t        rex = 0;
        const uint8_t* q   = p;
        if ((*q & 0xf0) == 0x40) {
            rex = *q & 0x0f;
            q++;
        }
        if (q < end && (*q & 0xf8) == 0x58) {
            // pop r64: 0x58+rd  (with REX.B for r8-r15)
            uint8_t   reg     = (*q & 0x07) | ((rex & 0x01) << 3);
            uintptr_t val     = 0;
            SIZE_T    read_sz = 0;
            ReadProcessMemory(hProcess, (LPCVOID)sp, &val, sizeof(val), &read_sz);
            gp[reg] = val;
            if (reg == REG_RBP)
                bp = val;
            sp += 8;
            p   = q + 1;
            continue;
        }
        break; // not a pop
    }

    // Must end with ret (0xc3) or jmp (0xeb, 0xe9, or 0xff /4 or /5).
    if (p >= end)
        return false;
    if (*p == 0xc3) {
        // ret: read return address from [rsp]
        uintptr_t ret_addr = 0;
        SIZE_T    read_sz  = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)sp, &ret_addr, sizeof(ret_addr), &read_sz)
            || read_sz != sizeof(ret_addr))
            return false;
        *rip        = ret_addr;
        gp[REG_RSP] = sp + 8;
        gp[REG_RBP] = bp;
        log_d(
            "win: epilog matched (ret) at pc=%" PRIxPTR " remaining=%zu -> ret=%" PRIxPTR " new_sp=%" PRIxPTR, pc,
            remaining, ret_addr, gp[REG_RSP]
        );
        return true;
    }
    if (*p == 0xeb || *p == 0xe9) {
        // jmp rel8/rel32 (tail call) — treat as end of stack for our purposes
        // We can't follow the jump target for unwinding, so just read [rsp].
        uintptr_t ret_addr = 0;
        SIZE_T    read_sz  = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)sp, &ret_addr, sizeof(ret_addr), &read_sz)
            || read_sz != sizeof(ret_addr))
            return false;
        *rip        = ret_addr;
        gp[REG_RSP] = sp + 8;
        gp[REG_RBP] = bp;
        log_d(
            "win: epilog matched (jmp) at pc=%" PRIxPTR " remaining=%zu -> ret=%" PRIxPTR " new_sp=%" PRIxPTR, pc,
            remaining, ret_addr, gp[REG_RSP]
        );
        return true;
    }
    if (*p == 0xff && p + 1 < end) {
        uint8_t modrm      = p[1];
        uint8_t mod_opcode = modrm & 0xf8;
        if (mod_opcode == 0x20 || mod_opcode == 0x28) {
            // jmp [r/m64] (indirect tail call)
            uintptr_t ret_addr = 0;
            SIZE_T    read_sz  = 0;
            if (!ReadProcessMemory(hProcess, (LPCVOID)sp, &ret_addr, sizeof(ret_addr), &read_sz)
                || read_sz != sizeof(ret_addr))
                return false;
            *rip        = ret_addr;
            gp[REG_RSP] = sp + 8;
            gp[REG_RBP] = bp;
            log_d(
                "win: epilog matched (jmp*/call*) at pc=%" PRIxPTR " remaining=%zu -> ret=%" PRIxPTR
                " new_sp=%" PRIxPTR,
                pc, remaining, ret_addr, gp[REG_RSP]
            );
            return true;
        }
    }

    log_d("win: epilog no match at pc=%" PRIxPTR " remaining=%zu (byte=0x%02x)", pc, remaining, (unsigned)*p);
    return false; // Not a valid epilog
}

static inline bool
_pe_unwind_step(
    pdata_cache_entry_t* ce, RUNTIME_FUNCTION* rf, uintptr_t pc, HANDLE hProcess, uintptr_t* rip,
    uintptr_t gp[GP_REG_COUNT]
) {
    pe_unwind_info_t* ui = _pdata_get_unwind_info(ce, rf);
    if (!ui) {
        log_d(
            "win: _pe_unwind_step: no unwind info for rva %x-%x (UnwindData=%x, xdata_rva=%" PRIxPTR " xdata_size=%zu)",
            rf->BeginAddress, rf->EndAddress, rf->UnwindData, ce->xdata_rva, ce->xdata_size
        );
        return false;
    }

    pe_unwind_info_t* current_ui = ui;
    RUNTIME_FUNCTION* current_rf = rf;
    uintptr_t         sp         = gp[REG_RSP];

    // Check for epilog before processing unwind codes.  If the PC is past the
    // prologue and we're in a valid epilog sequence, simulate the remaining
    // epilog instructions directly — unwind codes don't account for partial
    // epilog execution.
    {
        DWORD     func_start = rf->BeginAddress;
        DWORD     func_end   = rf->EndAddress;
        uintptr_t pc_in_func = pc - ce->image_base;
        if (pc_in_func >= func_start + ui->SizeOfProlog && pc_in_func < func_end) {
            // Save state before epilog attempt so we can roll back on
            // false positives.
            uintptr_t saved_rip = *rip;
            uintptr_t saved_gp[GP_REG_COUNT];
            memcpy(saved_gp, gp, sizeof(saved_gp));

            if (_pe_try_epilog(hProcess, pc, ce->image_base + func_end, ui->FrameRegister, rip, gp)) {
                // Validate: a real epilog produces a return address that
                // points to executable code.  Check that the address is in
                // a known module AND within the code range covered by that
                // module's .pdata entries (to reject data-section addresses).
                bool epilog_valid = false;
                if (*rip != 0 && _pc_in_module(*rip, _mod_table, _mod_count)) {
                    epilog_valid = true;
                    // Further check: if we have pdata for the target module,
                    // verify the rva is within the code range.
                    DWORD elo = 0, ehi = _mod_count;
                    while (elo < ehi) {
                        DWORD emid = elo + (ehi - elo) / 2;
                        if (_mod_table[emid].base <= *rip)
                            elo = emid + 1;
                        else
                            ehi = emid;
                    }
                    if (elo > 0) {
                        pdata_cache_entry_t* tgt_ce = _pdata_cache_lookup(_mod_table[elo - 1].base);
                        if (tgt_ce && tgt_ce->count > 0) {
                            DWORD ret_rva      = (DWORD)(*rip - tgt_ce->image_base);
                            DWORD max_code_rva = tgt_ce->funcs[tgt_ce->count - 1].EndAddress;
                            if (ret_rva > max_code_rva) {
                                epilog_valid = false;
                            } else {
                                // Require the return address to fall inside a
                                // known RUNTIME_FUNCTION.  An address that is
                                // in the module image but outside every .pdata
                                // entry (e.g. a data section or a gap) is
                                // almost certainly a false-positive epilog.
                                // Leaf call sites (no RUNTIME_FUNCTION) are
                                // still accepted because _pdata_find returning
                                // NULL for them is expected and harmless.
                                // We only reject when the ret_rva is strictly
                                // beyond the last function end yet within the
                                // image — that case is already caught above.
                                // The stricter check: if we DO have a cache
                                // entry and the address falls in a gap between
                                // functions (pdata_find returns NULL AND it is
                                // not a leaf, i.e. ret_rva <
                                // funcs[0].BeginAddress or in a mid-pdata
                                // gap), reject it.
                                if (ret_rva < tgt_ce->funcs[0].BeginAddress)
                                    epilog_valid = false;
                                else if (_pdata_find(tgt_ce, ret_rva) == NULL) {
                                    // ret_rva is between function entries —
                                    // either a leaf call site (acceptable) or
                                    // a gap (likely garbage).  Distinguish by
                                    // checking the new RSP makes sense: after
                                    // the epilog the RSP must be strictly
                                    // above the saved_gp RSP (at least +8 for
                                    // the popped return address, typically
                                    // much more).  If RSP didn't advance by
                                    // at least 16 bytes beyond the pre-step
                                    // RSP the epilog likely consumed no frame.
                                    if (gp[REG_RSP] < saved_gp[REG_RSP] + 16)
                                        epilog_valid = false;
                                }
                            }
                        }
                    }
                }
                if (epilog_valid) {
                    log_d(
                        "win: epilog detected at rva %x (func %x-%x), ret=%" PRIxPTR, (DWORD)pc_in_func, func_start,
                        func_end, *rip
                    );
                    return true;
                }
                // Roll back — false positive epilog detection.
                log_d(
                    "win: epilog FALSE POSITIVE at rva %x (func %x-%x), bad ret=%" PRIxPTR, (DWORD)pc_in_func,
                    func_start, func_end, *rip
                );
                *rip = saved_rip;
                memcpy(gp, saved_gp, sizeof(saved_gp));
            }
        }
    }

    bool is_chained = false;
    for (;;) {
        pe_unwind_code_t* codes      = (pe_unwind_code_t*)(current_ui + 1);
        int               code_count = current_ui->CountOfCodes;

        // Determine if we're in the prologue.  For chained unwind info, all
        // codes must be processed unconditionally (pe-unwind-info skips the
        // offset check with !is_chained).
        DWORD func_start = current_rf->BeginAddress;
        DWORD pc_offset  = (DWORD)(pc - (ce->image_base + func_start));
        bool  in_prolog  = !is_chained && (pc_offset < current_ui->SizeOfProlog);

        uint8_t frame_reg    = current_ui->FrameRegister;
        uint8_t frame_offset = current_ui->FrameOffset;

        for (int i = 0; i < code_count; /* advanced in body */) {
            pe_unwind_code_t* op = &codes[i];

            // In the prologue, skip codes that haven't executed yet.
            if (in_prolog && op->CodeOffset > pc_offset) {
                switch (op->UnwindOp) {
                case UWOP_ALLOC_LARGE:
                    i += (op->OpInfo == 0) ? 2 : 3;
                    break;
                case UWOP_SAVE_NONVOL:
                case UWOP_SAVE_XMM128:
                case 6: // UWOP_EPILOG
                    i += 2;
                    break;
                case UWOP_SAVE_NONVOL_FAR:
                case UWOP_SAVE_XMM128_FAR:
                    i += 3;
                    break;
                default:
                    i += 1;
                }
                continue;
            }

            switch (op->UnwindOp) {
            case UWOP_PUSH_NONVOL: {
                uintptr_t val     = 0;
                SIZE_T    read_sz = 0;
                ReadProcessMemory(hProcess, (LPCVOID)sp, &val, sizeof(val), &read_sz);
                gp[op->OpInfo]  = val;
                sp             += 8;
                i              += 1;
                break;
            }
            case UWOP_ALLOC_LARGE: {
                if (op->OpInfo == 0) {
                    sp += (uintptr_t)codes[i + 1].FrameOffset * 8;
                    i  += 2;
                } else {
                    uint32_t alloc  = *(uint32_t*)&codes[i + 1];
                    sp             += alloc;
                    i              += 3;
                }
                break;
            }
            case UWOP_ALLOC_SMALL: {
                sp += (uintptr_t)(op->OpInfo * 8 + 8);
                i  += 1;
                break;
            }
            case UWOP_SET_FPREG: {
                // Restore RSP from the frame register, matching pe-unwind-info's
                // RestoreSPFromFP: sp = gp[frame_reg] - frame_offset * 16.
                if (frame_reg != 0)
                    sp = gp[frame_reg] - (uintptr_t)frame_offset * 16;
                i += 1;
                break;
            }
            case UWOP_SAVE_NONVOL: {
                // resolve_offset: if frame register is set, base from it;
                // otherwise base from current RSP.
                uintptr_t offset = (uintptr_t)codes[i + 1].FrameOffset * 8;
                uintptr_t addr;
                if (frame_reg != 0)
                    addr = gp[frame_reg] - (uintptr_t)frame_offset * 16 + offset;
                else
                    addr = sp + offset;
                uintptr_t val     = 0;
                SIZE_T    read_sz = 0;
                ReadProcessMemory(hProcess, (LPCVOID)addr, &val, sizeof(val), &read_sz);
                gp[op->OpInfo]  = val;
                i              += 2;
                break;
            }
            case UWOP_SAVE_NONVOL_FAR: {
                uint32_t  offset = *(uint32_t*)&codes[i + 1];
                uintptr_t addr;
                if (frame_reg != 0)
                    addr = gp[frame_reg] - (uintptr_t)frame_offset * 16 + offset;
                else
                    addr = sp + offset;
                uintptr_t val     = 0;
                SIZE_T    read_sz = 0;
                ReadProcessMemory(hProcess, (LPCVOID)addr, &val, sizeof(val), &read_sz);
                gp[op->OpInfo]  = val;
                i              += 3;
                break;
            }
            case 6: // UWOP_EPILOG (v2) — epilog scope information; skip.
                i += 2;
                break;
            case UWOP_PUSH_MACHFRAME: {
                uintptr_t base    = sp + (op->OpInfo ? 8 : 0);
                SIZE_T    read_sz = 0;
                ReadProcessMemory(hProcess, (LPCVOID)base, rip, sizeof(*rip), &read_sz);
                ReadProcessMemory(hProcess, (LPCVOID)(base + 24), &gp[REG_RSP], sizeof(gp[REG_RSP]), &read_sz);
                return true;
            }
            case UWOP_SAVE_XMM128:
                i += 2;
                break;
            case UWOP_SAVE_XMM128_FAR:
                i += 3;
                break;
            default:
                return false;
            }
        }

        // Follow chained unwind info.
        if (current_ui->Flags & UNW_FLAG_CHAININFO) {
            int               aligned_count = (code_count + 1) & ~1;
            RUNTIME_FUNCTION* chained_rf    = (RUNTIME_FUNCTION*)&codes[aligned_count];
            current_rf                      = chained_rf;
            current_ui                      = _pdata_get_unwind_info(ce, chained_rf);
            if (!current_ui)
                return false;
            is_chained = true;
            continue;
        }

        break;
    }

    // After reversing the prologue, the return address is at [sp].
    uintptr_t ret_addr = 0;
    SIZE_T    read_sz  = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)sp, &ret_addr, sizeof(ret_addr), &read_sz) || read_sz != sizeof(ret_addr))
        return false;

    *rip        = ret_addr;
    gp[REG_RSP] = sp + 8; // pop the return address
    log_d(
        "win: _pe_unwind_step: rva %x-%x -> ret=%" PRIxPTR " sp=%" PRIxPTR, rf->BeginAddress, rf->EndAddress, ret_addr,
        sp + 8
    );
    return true;
}
#endif // _M_X64

// ---------------------------------------------------------------------------
// Public interface: step one frame using cached .pdata
// ---------------------------------------------------------------------------

// Step one native frame.  Looks up the module for `pc`, loads/caches its
// .pdata, finds the RUNTIME_FUNCTION, and interprets the unwind codes.
// On success, updates *pc and register state to the caller's values.
// Returns true on success.
static inline bool
pdata_step(HANDLE hProcess, uintptr_t* pc, uintptr_t gp[GP_REG_COUNT], _mod_entry_t* mod_table, DWORD mod_count) {
    // Find the module containing this PC (binary search).
    DWORD mlo = 0, mhi = mod_count;
    while (mlo < mhi) {
        DWORD mid = mlo + (mhi - mlo) / 2;
        if (mod_table[mid].base <= *pc)
            mlo = mid + 1;
        else
            mhi = mid;
    }
    bool in_module = (mlo > 0 && *pc >= mod_table[mlo - 1].base && *pc < mod_table[mlo - 1].end);
    if (!in_module) {
        // PC is outside any known module.  We can't reliably unwind from here
        // (no .pdata to consult), so return false and let the StackWalk64
        // fallback handle it.
        return false;
    }
    _mod_entry_t* mod = &mod_table[mlo - 1];

    uintptr_t            image_base = mod->base;
    pdata_cache_entry_t* ce         = _pdata_cache_lookup(image_base);
    if (!ce)
        ce = _pdata_cache_load(hProcess, image_base);
    if (!ce) {
        log_d("win: pdata_step: no pdata cache for module at %" PRIxPTR " (%s)", image_base, mod->path);
        return false;
    }

    DWORD             rva     = (DWORD)(*pc - image_base);
    RUNTIME_FUNCTION* rt_func = _pdata_find(ce, rva);

    if (rt_func == NULL) {
        log_d("win: pdata_step: no RUNTIME_FUNCTION for rva %x in %s (leaf)", rva, mod->path);
        // Leaf function: no unwind info.
        uintptr_t ret_addr  = 0;
        SIZE_T    read_size = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)gp[REG_RSP], &ret_addr, sizeof(ret_addr), &read_size)
            || read_size != sizeof(ret_addr))
            return false;
        log_d(
            "win: pdata_step: leaf %" PRIxPTR " -> ret=%" PRIxPTR " new_sp=%" PRIxPTR, *pc, ret_addr,
            gp[REG_RSP] + sizeof(uintptr_t)
        );
        *pc          = ret_addr;
        gp[REG_RSP] += sizeof(uintptr_t);
        return true;
    }

    log_d(
        "win: pdata_step: pc=%" PRIxPTR " rva=%x rf=[%x,%x) in %s", *pc, rva, rt_func->BeginAddress,
        rt_func->EndAddress, mod->path
    );
    uintptr_t old_pc = *pc;
    bool      ok     = _pe_unwind_step(ce, rt_func, old_pc, hProcess, pc, gp);
    if (ok)
        log_d("win: pdata_step: unwound %" PRIxPTR " -> %" PRIxPTR " new_sp=%" PRIxPTR, old_pc, *pc, gp[REG_RSP]);
    return ok;
}

// ---------------------------------------------------------------------------
// Custom StackWalk64 callbacks backed by our local pdata/module caches
// ---------------------------------------------------------------------------
//
// SymFunctionTableAccess64 depends on DbgHelp having loaded each module's
// exception directory.  With SYMOPT_DEFERRED_LOADS that may not have happened
// yet when a module is first encountered, causing StackWalk64 to fall back to
// frame-pointer unwinding and produce garbage frames in optimised x64 code.
//
// These callbacks consult our own pdata cache first (already populated from
// the target process via ReadProcessMemory), then fall through to DbgHelp for
// any module we haven't seen yet.  Because we use the same cache as the fast
// pdata walker, StackWalk64 effectively gets the same function table coverage
// we do — but uses its own (battle-tested) unwind-opcode interpreter, which
// may handle edge cases our walker misses.

#if defined(_M_X64)

static PVOID CALLBACK
_custom_function_table_access64(HANDLE hProcess, DWORD64 addr_base) {
    uintptr_t pc  = (uintptr_t)addr_base;
    DWORD     mlo = 0, mhi = _mod_count;
    while (mlo < mhi) {
        DWORD mid = mlo + (mhi - mlo) / 2;
        if (_mod_table[mid].base <= pc)
            mlo = mid + 1;
        else
            mhi = mid;
    }
    if (mlo > 0 && pc >= _mod_table[mlo - 1].base && pc < _mod_table[mlo - 1].end) {
        uintptr_t            image_base = _mod_table[mlo - 1].base;
        pdata_cache_entry_t* ce         = _pdata_cache_lookup(image_base);
        if (!ce)
            ce = _pdata_cache_load(hProcess, image_base);
        if (ce) {
            DWORD             rva = (DWORD)(pc - image_base);
            RUNTIME_FUNCTION* rf  = _pdata_find(ce, rva);
            // For leaf functions rf is NULL: return NULL so StackWalk64 reads
            // the return address from [RSP] directly, which is correct.
            return (PVOID)rf;
        }
    }
    // Module not in our table yet — fall back to DbgHelp.
    return SymFunctionTableAccess64(hProcess, addr_base);
}

static DWORD64 CALLBACK
_custom_get_module_base64(HANDLE hProcess, DWORD64 addr) {
    uintptr_t pc  = (uintptr_t)addr;
    DWORD     mlo = 0, mhi = _mod_count;
    while (mlo < mhi) {
        DWORD mid = mlo + (mhi - mlo) / 2;
        if (_mod_table[mid].base <= pc)
            mlo = mid + 1;
        else
            mhi = mid;
    }
    if (mlo > 0 && pc >= _mod_table[mlo - 1].base && pc < _mod_table[mlo - 1].end)
        return (DWORD64)_mod_table[mlo - 1].base;
    // Not in our table — fall back to DbgHelp.
    return SymGetModuleBase64(hProcess, addr);
}

#endif // _M_X64
