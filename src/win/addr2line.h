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

// Win32 native symbol resolution for austin on Windows.
//
// Provides get_func_name() which maps a program counter to the enclosing
// function name via DbgHelp (SymFromAddr), and get_module_name() which
// maps a PC to the owning module path using a cached, sorted module table
// built from EnumProcessModulesEx.
//
// DbgHelp is initialised lazily on first use.

#pragma once

#include <dbghelp.h>
#include <psapi.h>

#include "../error.h"
#include "../hints.h"
#include "../logging.h"

// ---- DbgHelp initialisation ------------------------------------------------

static HANDLE _dbghelp_proc = NULL;

static inline void
sym_init(HANDLE hProcess) {
    if (_dbghelp_proc == hProcess)
        return; // already initialised for this process

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(hProcess, NULL, TRUE)) {
        log_e("SymInitialize failed (error %lu)", GetLastError());
        return;
    }
    _dbghelp_proc = hProcess;
}

static inline void
sym_cleanup(void) {
    if (_dbghelp_proc != NULL) {
        SymCleanup(_dbghelp_proc);
        _dbghelp_proc = NULL;
    }
}

// ---- Function name resolution via DbgHelp ----------------------------------

// Static buffer for SymFromAddr.  DbgHelp is not thread-safe per process
// handle, but Austin's sampling loop is single-threaded so this is fine.
static char _sym_buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];

// Check whether the module containing `pc` has full (PDB) symbols loaded
// or only export symbols.  Returns true if PDB/DIA symbols are available.
static inline bool
_has_pdb_symbols(HANDLE hProcess, uintptr_t pc) {
    IMAGEHLP_MODULE64 mod_info;
    memset(&mod_info, 0, sizeof(mod_info));
    mod_info.SizeOfStruct = sizeof(mod_info);

    if (!SymGetModuleInfo64(hProcess, (DWORD64)pc, &mod_info))
        return false;

    // SymPdb (7) and SymDia (8) indicate full symbol info from PDB files.
    return mod_info.SymType == SymPdb || mod_info.SymType == SymDia;
}

static inline const char*
get_func_name(HANDLE hProcess, uintptr_t pc) {
    sym_init(hProcess);

    SYMBOL_INFO* sym  = (SYMBOL_INFO*)_sym_buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (!SymFromAddr(hProcess, (DWORD64)pc, &displacement, sym)) {
        return NULL;
    }

    // With full PDB symbols the resolution is exact — trust it.
    // Without PDBs (export-only), SymFromAddr maps the PC to the nearest
    // preceding export which can be wildly wrong.  Validate that the PC
    // actually falls within the reported symbol's bounds.
    if (!_has_pdb_symbols(hProcess, pc)) {
        if (sym->Size > 0) {
            if (displacement >= sym->Size)
                return NULL;
        } else if (displacement > 0x10000) {
            return NULL;
        }
    }

    return sym->Name;
}

// ---- Cached module table for PC → module path lookups ----------------------

#define _MAX_MODULES 512

typedef struct {
    uintptr_t base;
    uintptr_t end;
    char      path[MAX_PATH];
} _mod_entry_t;

static _mod_entry_t _mod_table[_MAX_MODULES];
static DWORD        _mod_count = 0;
static HANDLE       _mod_proc  = NULL;

// Check if a PC falls within any known module (binary search).
static inline bool
_pc_in_module(uintptr_t pc, _mod_entry_t* mod_table, DWORD mod_count) {
    DWORD lo = 0, hi = mod_count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (mod_table[mid].base <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo > 0 && pc >= mod_table[lo - 1].base && pc < mod_table[lo - 1].end);
}

// Comparison function for qsort/bsearch: sort by base address.
static int
_mod_cmp(const void* a, const void* b) {
    uintptr_t ka = ((_mod_entry_t*)a)->base;
    uintptr_t kb = ((_mod_entry_t*)b)->base;
    return (ka > kb) - (ka < kb);
}

// Build (or rebuild) the sorted module table for the given process.
static inline void
modules_refresh(HANDLE hProcess) {
    HMODULE modules[_MAX_MODULES];
    DWORD   needed = 0;

    _mod_count = 0;
    _mod_proc  = hProcess;

    if (!EnumProcessModulesEx(hProcess, modules, sizeof(modules), &needed, LIST_MODULES_ALL))
        return;

    DWORD count = needed / sizeof(HMODULE);
    if (count > _MAX_MODULES)
        count = _MAX_MODULES;

    for (DWORD i = 0; i < count; i++) {
        MODULEINFO mi;
        if (!GetModuleInformation(hProcess, modules[i], &mi, sizeof(mi)))
            continue;

        _mod_entry_t* e = &_mod_table[_mod_count];
        e->base         = (uintptr_t)mi.lpBaseOfDll;
        e->end          = e->base + mi.SizeOfImage;

        if (GetModuleFileNameExA(hProcess, modules[i], e->path, MAX_PATH) > 0)
            _mod_count++;
    }

    qsort(_mod_table, _mod_count, sizeof(_mod_entry_t), _mod_cmp);
    log_d("win: cached %lu module entries", (unsigned long)_mod_count);
}

// Binary search the cached module table for a PC.  Returns the module path
// or NULL if the PC doesn't fall within any cached range.
static inline const char*
_mod_lookup(uintptr_t pc) {
    DWORD lo = 0, hi = _mod_count;
    while (lo < hi) {
        DWORD mid = lo + (hi - lo) / 2;
        if (_mod_table[mid].base <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }

    if (lo == 0)
        return NULL;

    _mod_entry_t* e = &_mod_table[lo - 1];
    if (pc >= e->base && pc < e->end)
        return e->path;

    return NULL;
}

// Map a PC to the path of the loaded module that contains it.
// Uses a binary search over a cached sorted module table.  On a miss the
// table is rebuilt once (a new module may have been loaded lazily) and the
// lookup is retried.
static inline const char*
get_module_name(HANDLE hProcess, uintptr_t pc) {
    if (_mod_proc != hProcess || _mod_count == 0)
        modules_refresh(hProcess);

    const char* path = _mod_lookup(pc);
    if (path != NULL)
        return path;

    // Miss — a module may have been loaded since we last scanned.  Rebuild
    // the table once and retry.
    log_d("win: module lookup miss for PC %" PRIxPTR ", refreshing table", pc);
    modules_refresh(hProcess);

    path = _mod_lookup(pc);
    if (path == NULL)
        log_d("win: PC %" PRIxPTR " not found in any module after refresh", pc);

    return path;
}
