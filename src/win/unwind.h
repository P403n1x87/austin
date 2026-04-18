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
// Supports x64 and ARM64 unwind codes.

#pragma once

#include <windows.h>

#include "../logging.h"

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
                DWORD target_rva = funcs[0].UnwindData;
                for (DWORD i = 0; i < num_sections; i++) {
                    DWORD sec_start = sections[i].VirtualAddress;
                    DWORD sec_end   = sec_start + sections[i].Misc.VirtualSize;
                    if (target_rva >= sec_start && target_rva < sec_end) {
                        xdata_rva  = sec_start;
                        xdata_size = sections[i].Misc.VirtualSize;
                        xdata      = (uint8_t*)malloc(xdata_size);
                        if (xdata) {
                            if (!ReadProcessMemory(hProcess, (LPCVOID)(image_base + sec_start), xdata, xdata_size, &n)
                                || n != xdata_size) {
                                free(xdata);
                                xdata      = NULL;
                                xdata_size = 0;
                                xdata_rva  = 0;
                            }
                        }
                        break;
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
// Returns true if an epilog was detected and simulated (rsp/rip/rbp updated).
// Returns false if not in an epilog (caller should use unwind codes).
static inline bool
_pe_try_epilog(
    HANDLE hProcess, uintptr_t pc, uintptr_t func_end, uint8_t frame_reg, uintptr_t* rsp, uintptr_t* rip, uintptr_t* rbp
) {
    // Read up to 128 bytes of instructions from pc to func_end.
    size_t  remaining = (size_t)(func_end - pc);
    uint8_t ibuf[128];
    if (remaining == 0 || remaining > sizeof(ibuf))
        return false;

    SIZE_T n = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)pc, ibuf, remaining, &n) || n != remaining)
        return false;

    const uint8_t* p   = ibuf;
    const uint8_t* end = ibuf + remaining;
    uintptr_t      sp  = *rsp;
    uintptr_t      bp  = *rbp;

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
                        sp           = (frame_reg == 5) ? bp + disp8 : sp;
                        p            = q + 3;
                    } else if (mod_ == 0x02 && q + 6 <= end) {
                        int32_t disp32;
                        memcpy(&disp32, q + 2, 4);
                        sp = (frame_reg == 5) ? bp + disp32 : sp;
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
            if (reg == 5) // RBP
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
        *rip = ret_addr;
        *rsp = sp + 8;
        *rbp = bp;
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
        *rip = ret_addr;
        *rsp = sp + 8;
        *rbp = bp;
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
            *rip = ret_addr;
            *rsp = sp + 8;
            *rbp = bp;
            return true;
        }
    }

    return false; // Not a valid epilog
}

static inline bool
_pe_unwind_step(
    pdata_cache_entry_t* ce, RUNTIME_FUNCTION* rf, uintptr_t pc, HANDLE hProcess, uintptr_t* rsp, uintptr_t* rip,
    uintptr_t* rbp
) {
    pe_unwind_info_t* ui = _pdata_get_unwind_info(ce, rf);
    if (!ui)
        return false;

    pe_unwind_info_t* current_ui = ui;
    RUNTIME_FUNCTION* current_rf = rf;
    uintptr_t         sp         = *rsp;

    // Check for epilog before processing unwind codes.  If the PC is past the
    // prologue and we're in a valid epilog sequence, simulate the remaining
    // epilog instructions directly — unwind codes don't account for partial
    // epilog execution.
    {
        DWORD     func_start = rf->BeginAddress;
        DWORD     func_end   = rf->EndAddress;
        uintptr_t pc_in_func = pc - ce->image_base;
        if (pc_in_func >= func_start + ui->SizeOfProlog && pc_in_func < func_end) {
            if (_pe_try_epilog(hProcess, pc, ce->image_base + func_end, ui->FrameRegister, rsp, rip, rbp))
                return true;
        }
    }

    for (;;) {
        pe_unwind_code_t* codes      = (pe_unwind_code_t*)(current_ui + 1);
        int               code_count = current_ui->CountOfCodes;

        // Determine if we're in the prologue.
        DWORD   func_start = current_rf->BeginAddress;
        uint8_t pc_offset  = (uint8_t)(pc - (ce->image_base + func_start));
        bool    in_prolog  = (pc_offset < current_ui->SizeOfProlog);

        // If the function uses a frame pointer, recover RSP from it.
        if (current_ui->FrameRegister != 0) {
            if (current_ui->FrameRegister == 5 /* RBP */) {
                sp = *rbp - (uintptr_t)current_ui->FrameOffset * 16;
            }
        }

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
                if (op->OpInfo == 5 /* RBP */)
                    *rbp = val;
                sp += 8;
                i  += 1;
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
                i += 1;
                break;
            }
            case UWOP_SAVE_NONVOL: {
                if (op->OpInfo == 5 /* RBP */) {
                    // When a frame register is set, saved register offsets are
                    // relative to the established frame pointer value (FP - scaled
                    // FrameOffset).  Otherwise they are relative to RSP.
                    uintptr_t save_offset = (uintptr_t)codes[i + 1].FrameOffset * 8;
                    uintptr_t base
                        = (current_ui->FrameRegister != 0) ? (*rbp - (uintptr_t)current_ui->FrameOffset * 16) : sp;
                    SIZE_T read_sz = 0;
                    ReadProcessMemory(hProcess, (LPCVOID)(base + save_offset), rbp, sizeof(*rbp), &read_sz);
                }
                i += 2;
                break;
            }
            case UWOP_SAVE_NONVOL_FAR: {
                if (op->OpInfo == 5 /* RBP */) {
                    uint32_t  save_offset = *(uint32_t*)&codes[i + 1];
                    uintptr_t base
                        = (current_ui->FrameRegister != 0) ? (*rbp - (uintptr_t)current_ui->FrameOffset * 16) : sp;
                    SIZE_T read_sz = 0;
                    ReadProcessMemory(hProcess, (LPCVOID)(base + save_offset), rbp, sizeof(*rbp), &read_sz);
                }
                i += 3;
                break;
            }
            case UWOP_PUSH_MACHFRAME: {
                uintptr_t base    = sp + (op->OpInfo ? 8 : 0);
                SIZE_T    read_sz = 0;
                ReadProcessMemory(hProcess, (LPCVOID)base, rip, sizeof(*rip), &read_sz);
                ReadProcessMemory(hProcess, (LPCVOID)(base + 24), rsp, sizeof(*rsp), &read_sz);
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
            continue;
        }

        break;
    }

    // After reversing the prologue, the return address is at [sp].
    uintptr_t ret_addr = 0;
    SIZE_T    read_sz  = 0;
    if (!ReadProcessMemory(hProcess, (LPCVOID)sp, &ret_addr, sizeof(ret_addr), &read_sz) || read_sz != sizeof(ret_addr))
        return false;

    *rip = ret_addr;
    *rsp = sp + 8; // pop the return address
    return true;
}
#endif // _M_X64

// ---------------------------------------------------------------------------
// Userspace PE unwind — ARM64
// ---------------------------------------------------------------------------
//
// ARM64 PE unwind data uses a packed variable-length header followed by
// byte-coded unwind opcodes.  The key difference from x64: the return
// address lives in the Link Register (LR / x30), not on the stack.
//
// We need to recover: SP, PC (from LR), and optionally FP (x29).

#if defined(_M_ARM64)

// ARM64 unwind header (first 32-bit word of .xdata record).
typedef struct {
    uint32_t FunctionLength : 18;
    uint32_t Version : 2;
    uint32_t X : 1; // exception handler present
    uint32_t E : 1; // single packed epilog
    uint32_t EpilogCount : 5;
    uint32_t CodeWords : 5;
} arm64_unwind_header_t;

static inline bool
_pe_unwind_step_arm64(
    pdata_cache_entry_t* ce, RUNTIME_FUNCTION* rf, uintptr_t pc, HANDLE hProcess, uintptr_t* sp_out, uintptr_t* pc_out,
    uintptr_t* fp_out, uintptr_t* lr_out
) {
    // ARM64 .xdata is at the UnwindData RVA.
    uint8_t* xdata = (uint8_t*)_pdata_get_xdata(ce, rf->UnwindData, sizeof(arm64_unwind_header_t));
    if (!xdata)
        return false;

    arm64_unwind_header_t hdr;
    memcpy(&hdr, xdata, sizeof(hdr));

    // Skip the header word(s) and epilog scopes to reach the unwind codes.
    const uint8_t* codes;
    uint32_t       code_words   = hdr.CodeWords;
    uint32_t       epilog_count = hdr.EpilogCount;

    // Check for extended header (EpilogCount==0 && CodeWords==0).
    size_t header_words = 1;
    if (epilog_count == 0 && code_words == 0) {
        // Extended header: second word has extended counts.
        uint8_t* ext = (uint8_t*)_pdata_get_xdata(ce, rf->UnwindData, 8);
        if (!ext)
            return false;
        uint32_t word2;
        memcpy(&word2, ext + 4, sizeof(word2));
        epilog_count = word2 & 0xFFFF;
        code_words   = (word2 >> 16) & 0xFF;
        header_words = 2;
    }

    // Epilog scopes follow the header (each is 1 word), except when E==1
    // (single packed epilog — no separate scope records).
    size_t epilog_scope_words = hdr.E ? 0 : epilog_count;
    size_t codes_offset       = (header_words + epilog_scope_words) * 4;

    codes = (uint8_t*)_pdata_get_xdata(ce, rf->UnwindData, codes_offset + code_words * 4);
    if (!codes)
        return false;
    codes += codes_offset;

    const uint8_t* codes_end = codes + code_words * 4;

    uintptr_t sp = *sp_out;
    uintptr_t fp = *fp_out;
    uintptr_t lr = *lr_out;

    // Determine if we are in the prologue.
    DWORD    func_start = rf->BeginAddress;
    uint32_t func_len   = hdr.FunctionLength * 4;
    uint32_t pc_offset  = (uint32_t)(pc - (ce->image_base + func_start));
    (void)func_len;
    // We don't do fine-grained prologue offset tracking for ARM64 — we assume
    // the full prologue has executed unless pc == func_start.  This is safe
    // because the ARM64 ABI guarantees atomic prologue/epilogue sequences.
    bool fully_in_body = (pc_offset > 0);
    (void)fully_in_body;

    // Interpret unwind codes.
    const uint8_t* p = codes;
    while (p < codes_end) {
        uint8_t b = *p;

        if (b == 0xE4) {
            // end — stop processing.
            break;
        }
        if (b == 0xE5) {
            // end_c — end of chained scope.
            break;
        }

        if ((b & 0xE0) == 0x00) {
            // alloc_s: 000xxxxx — allocate (x * 16) bytes
            sp += (uintptr_t)(b & 0x1F) * 16;
            p  += 1;
        } else if ((b & 0xE0) == 0x20) {
            // save_r19r20_x: 001zzzzz — pre-indexed save <x19,x20>
            // Not needed for stack walking (we only care about fp/lr/sp).
            // Advance sp by the pre-index amount: (z+1)*8 for the pair.
            // Actually this is stp x19,x20,[sp,#-(z+1)*8]! so it decrements sp.
            // During unwind we reverse it: sp += (z+1)*8.  But we already
            // account for the allocation separately; the save just stores regs.
            // Skip — we don't track x19/x20.
            p += 1;
        } else if ((b & 0xC0) == 0x40) {
            // save_fplr: 01zzzzzz — save <x29,lr> at [sp + z*8]
            // Read lr from the saved location.
            uintptr_t offset  = (uintptr_t)(b & 0x3F) * 8;
            SIZE_T    read_sz = 0;
            uintptr_t pair[2] = {0, 0};
            ReadProcessMemory(hProcess, (LPCVOID)(sp + offset), pair, sizeof(pair), &read_sz);
            fp  = pair[0]; // x29
            lr  = pair[1]; // x30/lr
            p  += 1;
        } else if ((b & 0xC0) == 0x80) {
            // save_fplr_x: 10zzzzzz — pre-indexed save <x29,lr>
            // stp x29,lr,[sp,#-(z+1)*8]!
            // Reverse: sp += (z+1)*8, then read pair from [sp - (z+1)*8] = old sp.
            uintptr_t alloc   = (uintptr_t)((b & 0x3F) + 1) * 8;
            SIZE_T    read_sz = 0;
            uintptr_t pair[2] = {0, 0};
            ReadProcessMemory(hProcess, (LPCVOID)sp, pair, sizeof(pair), &read_sz);
            fp  = pair[0];
            lr  = pair[1];
            sp += alloc;
            p  += 1;
        } else if (b == 0xE1) {
            // set_fp: mov x29, sp — frame pointer was set.
            // During unwind: sp = fp.
            sp  = fp;
            p  += 1;
        } else if (b == 0xE6) {
            // save_next — save next register pair (skip, not needed).
            p += 1;
        } else if ((b & 0xF8) == 0xC0) {
            // alloc_m: 11000xxx xxxxxxxx — allocate x * 16 bytes (2 bytes)
            if (p + 1 >= codes_end)
                break;
            uint32_t alloc  = (((uint32_t)(b & 0x07) << 8) | (uint32_t)p[1]) * 16;
            sp             += alloc;
            p              += 2;
        } else if ((b & 0xFC) == 0xC8) {
            // save_regp: 110010xx xxxxxxxx — save pair x(19+i) (2 bytes)
            // Skip — we don't track general-purpose registers other than fp/lr.
            p += 2;
        } else if ((b & 0xFC) == 0xCC) {
            // save_regp_x: 110011xx xxxxxxxx — pre-indexed save pair (2 bytes)
            p += 2;
        } else if ((b & 0xFC) == 0xD0) {
            // save_reg: 110100xx xxxxxxxx — save single register (2 bytes)
            p += 2;
        } else if ((b & 0xFE) == 0xD4) {
            // save_reg_x: 1101010x xxxxxxxx — pre-indexed save single (2 bytes)
            p += 2;
        } else if ((b & 0xFE) == 0xD6) {
            // save_lrpair: 1101011x xxxxxxxx — save <r, lr> pair (2 bytes)
            // This saves lr — read it.
            if (p + 1 >= codes_end)
                break;
            uint8_t   reg    = (uint8_t)(2 * ((b & 0x01) << 3 | (p[1] >> 5)));
            uintptr_t offset = (uintptr_t)(p[1] & 0x1F) * 8;
            (void)reg;
            SIZE_T    read_sz = 0;
            uintptr_t pair[2] = {0, 0};
            ReadProcessMemory(hProcess, (LPCVOID)(sp + offset), pair, sizeof(pair), &read_sz);
            // pair[1] is lr
            lr  = pair[1];
            p  += 2;
        } else if ((b & 0xFE) == 0xD8) {
            // save_fregp: 1101100x xxxxxxxx — save FP pair d(8+i) (2 bytes)
            p += 2;
        } else if ((b & 0xFE) == 0xDA) {
            // save_fregp_x: 1101101x xxxxxxxx — pre-indexed save FP pair (2 bytes)
            p += 2;
        } else if ((b & 0xFE) == 0xDC) {
            // save_freg: 1101110x xxxxxxxx — save single FP register (2 bytes)
            p += 2;
        } else if (b == 0xDE) {
            // save_freg_x: 11011110 xxxxxxxx — pre-indexed save single FP (2 bytes)
            p += 2;
        } else if (b == 0xE0) {
            // alloc_l: 11100000 + 3 bytes — large allocation
            if (p + 3 >= codes_end)
                break;
            uint32_t alloc  = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            sp             += (uintptr_t)alloc * 16;
            p              += 4;
        } else if (b == 0xE2) {
            // context save/restore — skip.
            p += 1;
        } else if (b == 0xE3) {
            // context save/restore — skip.
            p += 1;
        } else if (b == 0xFC) {
            // pac_sign_lr — pointer authentication (ignore for unwinding).
            p += 1;
        } else {
            // Unknown opcode — skip one byte and hope for the best.
            log_d("win: unknown ARM64 unwind opcode 0x%02x", b);
            p += 1;
        }
    }

    *sp_out = sp;
    *fp_out = fp;
    *lr_out = lr;
    *pc_out = lr; // return address is in LR
    return true;
}
#endif // _M_ARM64

// ---------------------------------------------------------------------------
// Public interface: step one frame using cached .pdata
// ---------------------------------------------------------------------------

// Step one native frame.  Looks up the module for `pc`, loads/caches its
// .pdata, finds the RUNTIME_FUNCTION, and interprets the unwind codes.
// On success, updates *pc, *sp, *fp (and *lr on ARM64) to the caller's values.
// Returns true on success.
static inline bool
pdata_step(
    HANDLE hProcess, uintptr_t* pc, uintptr_t* sp, uintptr_t* fp,
#if defined(_M_ARM64)
    uintptr_t* lr,
#endif
    _mod_entry_t* mod_table, DWORD mod_count
) {
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
        // PC is outside any known module (e.g. syscall return stub, JIT code,
        // or trampoline).  Treat as a leaf function: read the return address
        // from [RSP] (x64) or use LR (ARM64).
#if defined(_M_X64)
        uintptr_t ret_addr  = 0;
        SIZE_T    read_size = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)*sp, &ret_addr, sizeof(ret_addr), &read_size)
            || read_size != sizeof(ret_addr) || ret_addr == 0)
            return false;
        *pc  = ret_addr;
        *sp += sizeof(uintptr_t);
#elif defined(_M_ARM64)
        if (*lr == 0)
            return false;
        *pc = *lr;
        *lr = 0;
#endif
        return true;
    }
    _mod_entry_t* mod = &mod_table[mlo - 1];

    uintptr_t            image_base = mod->base;
    pdata_cache_entry_t* ce         = _pdata_cache_lookup(image_base);
    if (!ce)
        ce = _pdata_cache_load(hProcess, image_base);
    if (!ce)
        return false;

    DWORD             rva     = (DWORD)(*pc - image_base);
    RUNTIME_FUNCTION* rt_func = _pdata_find(ce, rva);

    if (rt_func == NULL) {
        // Leaf function: no unwind info.
#if defined(_M_X64)
        uintptr_t ret_addr  = 0;
        SIZE_T    read_size = 0;
        if (!ReadProcessMemory(hProcess, (LPCVOID)*sp, &ret_addr, sizeof(ret_addr), &read_size)
            || read_size != sizeof(ret_addr))
            return false;
        *pc  = ret_addr;
        *sp += sizeof(uintptr_t);
#elif defined(_M_ARM64)
        if (*lr == 0)
            return false;
        *pc = *lr;
        *lr = 0;
#endif
        return true;
    }

#if defined(_M_X64)
    return _pe_unwind_step(ce, rt_func, *pc, hProcess, sp, pc, fp);
#elif defined(_M_ARM64)
    return _pe_unwind_step_arm64(ce, rt_func, *pc, hProcess, sp, pc, fp, lr);
#else
    return false;
#endif
}
