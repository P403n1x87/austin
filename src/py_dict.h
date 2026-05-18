// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

// Remote PyDict key-lookup helpers used for thread name resolution.
//
// py_dict__lookup_str  — find a string key; returns the value raddr.
// py_dict__entries     — expose the entries array so callers can walk it.
//
// Only "combined" (non-split, ma_values == NULL) dicts are handled.
// Uses struct definitions from python/dict.h to compute all offsets via
// offsetof, with no hardcoded magic numbers.
//
// copy_memory call budget per py_dict__entries invocation:
//   1  read PyDictBody.{ma_keys, ma_values} together (16 B)
//   1  read full PyDictKeysObject header (32 B for 3.11+, 40 B for 3.10)
//   ─────────────────────────────────────────────────────────────────────
//   2  total (down from 4–6 separate field reads)
//
// Per entry in py_dict__lookup_str:
//   1  read key_obj + val_obj together (16 B, always adjacent)

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hints.h"
#include "mem.h"
#include "py_string.h"
#include "python/dict.h"
#include "version.h"

// --- Index-entry byte size for 3.10 -----------------------------------------
// dk_size is the actual slot count (power of 2); index entries are 1/2/4/8 B.
static inline ssize_t
_dk_index_bytes_pre311(ssize_t dk_size) {
    if (dk_size <= 0xff)
        return 1;
    else if (dk_size <= 0xffff)
        return 2;
    else if (dk_size <= 0xffffffff)
        return 4;
    else
        return 8;
}

// ----------------------------------------------------------------------------
// Describes the entry array for a specific dict instance.
typedef struct {
    raddr_t start; // remote address of first entry
    ssize_t entry_size;
    ssize_t key_off; // key pointer offset within one entry
    ssize_t value_off;
    ssize_t nentries;
} dict_entries_t;

// ----------------------------------------------------------------------------
// Populate `out` from the PyDictObject at `dict_raddr`.
// Returns 0 on success, non-zero on error or split dict.
// 2 copy_memory calls total.
// ----------------------------------------------------------------------------
static inline int
py_dict__entries(proc_ref_t pref, raddr_t dict_raddr, python_v* py_v, dict_entries_t* out) {
    if (!isvalid(dict_raddr))
        return 1;

    ssize_t base = py_v->py_object_size;

    // Read ma_keys and ma_values in one call — they are adjacent in PyDictBody.
    raddr_t kv[2] = {NULL, NULL};
    if (fail(copy_memory(pref, dict_raddr + base + (ssize_t)offsetof(PyDictBody, ma_keys), sizeof(kv), kv)))
        return 1;
    raddr_t ma_keys   = kv[0];
    raddr_t ma_values = kv[1];
    if (!isvalid(ma_keys) || isvalid(ma_values))
        return 1; // split dict — skip

    ssize_t nentries   = 0;
    ssize_t nslots     = 0;
    ssize_t idx_bytes  = 0;
    ssize_t header     = 0;
    ssize_t entry_size = sizeof(PyDictKeyEntry);
    ssize_t key_off    = offsetof(PyDictKeyEntry, me_key);
    ssize_t value_off  = offsetof(PyDictKeyEntry, me_value);

    if (V_MIN(3, 11)) {
        // Compact layout (3.11+): read the full keys header in one call.
        PyDictKeysObject3_11 hdr;
        if (fail(copy_memory(pref, ma_keys, sizeof(hdr), &hdr)))
            return 1;
        nslots    = (ssize_t)1 << hdr.dk_log2_size;
        // dk_log2_index_bytes is log2 of the *total* index table byte count.
        idx_bytes = (ssize_t)1 << hdr.dk_log2_index_bytes;
        nentries  = hdr.dk_nentries;
        header    = sizeof(PyDictKeysObject3_11);
        if (hdr.dk_kind == DICT_KEYS_UNICODE) {
            entry_size = sizeof(PyDictUnicodeEntry);
            key_off    = offsetof(PyDictUnicodeEntry, me_key);
            value_off  = offsetof(PyDictUnicodeEntry, me_value);
        }
    } else {
        // 3.10: read the full keys header in one call.
        PyDictKeysObject3_10 hdr;
        if (fail(copy_memory(pref, ma_keys, sizeof(hdr), &hdr)))
            return 1;
        nslots    = hdr.dk_size;
        nentries  = hdr.dk_nentries;
        idx_bytes = _dk_index_bytes_pre311(nslots);
        header    = sizeof(PyDictKeysObject3_10);
    }

    if (nslots <= 0 || nslots > (1 << 20))
        return 1;
    if (nentries <= 0 || nentries > 65536)
        return 1;

    // For 3.11+: idx_bytes IS the total index table size; use directly.
    // For 3.10:  idx_bytes is per-slot; multiply by nslots.
    ssize_t total_idx = V_MIN(3, 11) ? idx_bytes : nslots * idx_bytes;
    out->start        = (raddr_t)((char*)ma_keys + header + total_idx);
    out->entry_size   = entry_size;
    out->key_off      = key_off;
    out->value_off    = value_off;
    out->nentries     = nentries;
    return 0;
}

// ----------------------------------------------------------------------------
// py_dict__log_keys: log all string keys found in a remote dict (debug only).
static inline void
py_dict__log_keys(proc_ref_t pref, raddr_t dict_raddr, python_v* py_v) {
    dict_entries_t e;
    if (py_dict__entries(pref, dict_raddr, py_v, &e)) {
        log_d("py_dict_keys: failed to read dict entries at %p", dict_raddr);
        return;
    }
    log_d("py_dict_keys: dict=%p nentries=%zd entry_size=%zd", dict_raddr, e.nentries, e.entry_size);
    for (ssize_t i = 0; i < e.nentries && i < 64; i++) {
        raddr_t entry   = (raddr_t)((char*)e.start + i * e.entry_size);
        raddr_t key_obj = NULL;
        if (fail(copy_memory(pref, entry + e.key_off, sizeof(raddr_t), &key_obj)))
            continue;
        if (!isvalid(key_obj))
            continue;
        char* k = _string_remote(pref, key_obj, py_v);
        if (isvalid(k)) {
            log_d("py_dict_keys:   [%zd] %s", i, k);
            free(k);
        } else {
            log_d("py_dict_keys:   [%zd] <non-string key_obj=%p>", i, key_obj);
        }
    }
}

// ----------------------------------------------------------------------------
// py_dict__lookup_str: find a C string key in a remote combined PyDict.
// Returns the remote value raddr, or NULL if not found / on error.
// 1 copy_memory call per entry (key_obj and val_obj read together).
// ----------------------------------------------------------------------------
static inline raddr_t
py_dict__lookup_str(proc_ref_t pref, raddr_t dict_raddr, const char* key, python_v* py_v) {
    dict_entries_t e;
    if (py_dict__entries(pref, dict_raddr, py_v, &e))
        return NULL;

    for (ssize_t i = 0; i < e.nentries; i++) {
        raddr_t entry = (raddr_t)((char*)e.start + i * e.entry_size);

        // key_off and value_off are always adjacent pointers — read both at once.
        raddr_t pair[2] = {NULL, NULL};
        if (fail(copy_memory(pref, entry + e.key_off, sizeof(pair), pair)))
            continue;
        raddr_t key_obj = pair[0];
        raddr_t val_obj = pair[1];
        if (!isvalid(key_obj) || !isvalid(val_obj))
            continue;

        char* k = _string_remote(pref, key_obj, py_v);
        if (!isvalid(k))
            continue;
        int match = (strcmp(k, key) == 0);
        free(k);
        if (match)
            return val_obj;
    }
    return NULL;
}
