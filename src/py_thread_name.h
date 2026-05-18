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

// Thread name resolution via the CPython threading module.
//
// Traversal path:
//   interp->modules (3.10-3.11, 3.13+) or interp->sysdict["modules"] (3.12)
//     = sys.modules dict
//     → "threading" module object
//       → module.__dict__["_active"]  (dict: ident → Thread)
//         → walk _active for matching ident
//           → Thread._name (string)
//
// Results are cached:
//   - threading._active raddr on interpreter_state_t (one per interpreter)
//   - the resolved name on thread_tracker_entry_t (one per thread lifetime)
//
// If the threading module has not been imported yet, the entry is marked
// NAME_NO_THREADING so the next sample retries.  If exactly one thread is
// alive and threading is absent, it is named "MainThread" unconditionally.
//
// Thread instance attribute access:
//   Python 3.11+ with Py_TPFLAGS_MANAGED_DICT stores Thread attributes as
//   "inline values" (split dict) before the __dict__ is explicitly accessed.
//   In that case obj[-3] (dict ptr) is NULL and obj[-4] (values ptr) is set.
//   We scan the first few inline values for the first unicode string, which
//   is always Thread._name (set as the second attribute in Thread.__init__,
//   right after _target).  If/when the dict is materialized, we use it
//   directly via py_dict__lookup_str(inst_dict, "_name").

#pragma once

#include <string.h>

#include "hints.h"
#include "logging.h"
#include "mem.h"
#include "py_dict.h"
#include "py_interp.h"
#include "py_long.h"
#include "py_string.h"
#include "thread_tracker.h"
#include "version.h"

// Target pointer size: 4 for 32-bit Python (py_object_size == 8), 8 otherwise.
// Used for PyTypeObject field offsets which are all pointer-sized.
#define _TARGET_PTR_SIZE(py_v) ((py_v)->py_object_size <= 8 ? (ssize_t)4 : (ssize_t)8)

// tp_flags is at ob_type + PyObject_VAR_HEAD size + 18 ptr-sized fields.
// PyObject_VAR_HEAD = PyObject_HEAD (py_object_size) + ob_size (1 ptr-sized field).
#define _TP_FLAGS_OFF(py_v)      (_TARGET_PTR_SIZE(py_v) * (1 + 18) + (py_v)->py_object_size)
// tp_dictoffset is at ob_type + PyObject_VAR_HEAD size + 33 ptr-sized fields.
#define _TP_DICTOFFSET_OFF(py_v) (_TARGET_PTR_SIZE(py_v) * (1 + 33) + (py_v)->py_object_size)

// Py_TPFLAGS_UNICODE_SUBCLASS — set on PyUnicode_Type and subclasses since CPython 3.1.
// Used to filter non-string objects from inline-values scans without executing Python.
#define _PY_TPFLAGS_UNICODE_SUBCLASS (1UL << 28)

// Read a target-process pointer (4 or 8 bytes depending on target architecture)
// from `addr`, zero-extended to raddr_t.  Handles cross-arch (64-bit Austin,
// 32-bit Python) correctly.
static inline raddr_t
_read_tgt_ptr(proc_ref_t pref, raddr_t addr, python_v* py_v) {
    ssize_t ps = _TARGET_PTR_SIZE(py_v);
    if (ps == (ssize_t)sizeof(raddr_t)) {
        raddr_t result = NULL;
        copy_memory(pref, addr, sizeof(raddr_t), &result);
        return result;
    }
    // 32-bit target pointer: read 4 bytes and zero-extend.
    uint32_t v = 0;
    if (success(copy_memory(pref, addr, 4, &v)))
        return (raddr_t)(uintptr_t)v;
    return NULL;
}

// Read ob_type from a PyObject and check Py_TPFLAGS_UNICODE_SUBCLASS.
// Returns true if the object is a unicode string (or subclass).
static inline bool
_is_unicode_object(proc_ref_t pref, raddr_t obj, python_v* py_v) {
    ssize_t ps      = _TARGET_PTR_SIZE(py_v);
    // ob_type is the last pointer in PyObject_HEAD: at offset py_object_size - ptr_size.
    raddr_t ob_type = _read_tgt_ptr(pref, obj + py_v->py_object_size - ps, py_v);
    if (!isvalid(ob_type))
        return false;
    unsigned long tp_flags = 0;
    if (fail(copy_memory(pref, ob_type + _TP_FLAGS_OFF(py_v), sizeof(tp_flags), &tp_flags)))
        return false;
    return (tp_flags & _PY_TPFLAGS_UNICODE_SUBCLASS) != 0;
}

// ----------------------------------------------------------------------------
// Walk threading._active to find the Thread whose ident matches `tid`.
// Returns a heap-allocated copy of Thread._name, or NULL.
// ----------------------------------------------------------------------------
static inline char*
_lookup_name_in_active(proc_ref_t pref, raddr_t active_dict, long tid, python_v* py_v) {
    dict_entries_t e;
    if (py_dict__entries(pref, active_dict, py_v, &e))
        return NULL;

    for (ssize_t i = 0; i < e.nentries; i++) {
        raddr_t entry_addr = (raddr_t)((char*)e.start + i * e.entry_size);
        // key_obj and val_obj are adjacent pointers — read both at once.
        raddr_t kv[2]      = {NULL, NULL};
        if (fail(copy_memory(pref, entry_addr + e.key_off, sizeof(kv), kv)))
            continue;
        raddr_t key_obj = kv[0];
        raddr_t val_obj = kv[1];
        if (!isvalid(key_obj) || !isvalid(val_obj))
            continue;

        long ident = py_long__read(pref, key_obj, py_v);
        log_d("thread name: _active[%zd] ident=0x%lx want=0x%lx", i, ident, tid);
        if (ident != tid)
            continue;

        // Found matching Thread object at val_obj.

        // 1. Get ob_type pointer (always the last pointer in PyObject_HEAD).
        ssize_t tps     = _TARGET_PTR_SIZE(py_v);
        raddr_t ob_type = _read_tgt_ptr(pref, val_obj + py_v->py_object_size - tps, py_v);
        if (!isvalid(ob_type)) {
            log_d("thread name: ob_type NULL/read failed");
            continue;
        }

        // 2. Read tp_flags.
        // Py_TPFLAGS_MANAGED_DICT  = 1 << 4  (3.11+)
        // Py_TPFLAGS_INLINE_VALUES = 1 << 2  (3.13+)
        unsigned long tp_flags = 0;
        if (fail(copy_memory(pref, ob_type + _TP_FLAGS_OFF(py_v), sizeof(tp_flags), &tp_flags)))
            continue;

        // 3. Locate Thread._name.  The mechanism differs by Python version:
        //
        // 3.10: traditional tp_dictoffset (positive).  Instance dicts are split:
        //       shared ma_keys + per-instance ma_values.  Walk ma_keys to find
        //       "_name" index, then read ma_values[index].
        //
        // 3.11: MANAGED_DICT.  obj[-3] = dict ptr, obj[-4] = values ptr.
        //
        // 3.12: DictOrValues at obj[-3].  (dorv & 1) == 1 means values mode;
        //       Python stores (values_ptr - 1) in the union, recover with (dorv + 1).
        //
        // 3.13+ GIL: obj[-24] = dict ptr; if NULL + INLINE_VALUES, values at obj+py_object_size.
        // 3.13+ FT:  obj[-8]  = dict ptr; inline values at obj+py_object_size.
        //
        // For all inline/values cases: scan up to 32 slots for the first unicode string
        // which is Thread._name (always set just after non-string attributes in __init__).

        raddr_t inst_dict = NULL;

        if (tp_flags & (1 << 4)) { // MANAGED_DICT

            if (V_MIN(3, 13)) {
                // GIL: MANAGED_DICT_OFFSET = -3*ptr
                // FT:  MANAGED_DICT_OFFSET = -1*ptr  (FT detected via larger py_object_size)
                ssize_t mdo      = (py_v->py_object_size > tps * 2) ? -tps : -3 * tps;
                raddr_t dict_ptr = _read_tgt_ptr(pref, (char*)val_obj + mdo, py_v);
                if (isvalid(dict_ptr)) {
                    inst_dict = dict_ptr;
                } else if (tp_flags & (1 << 2)) { // INLINE_VALUES
                    // Inline values at obj + tp_basicsize (= py_object_size here).
                    // PyDictValues3_13: 4-byte header (capacity, size, embedded, valid)
                    // + 4-byte padding + values[cap].  Read the whole block at once.
                    raddr_t iv = (raddr_t)((char*)val_obj + py_v->py_object_size);
                    // Max capacity we handle: 32 slots.  Header = 8 bytes, slots = 32*8 = 256.
                    char    iv_buf[8 + 32 * sizeof(raddr_t)];
                    ssize_t iv_read = sizeof(iv_buf);
                    if (fail(copy_memory(pref, iv, iv_read, iv_buf)))
                        continue;
                    uint8_t cap = ((PyDictValues3_13*)iv_buf)->capacity;
                    if (cap > 32)
                        cap = 32;
                    raddr_t* vals = (raddr_t*)((char*)iv_buf + offsetof(PyDictValues3_13, values[0]));
                    for (uint8_t k = 0; k < cap; k++) {
                        raddr_t vobj = vals[k];
                        if (!isvalid(vobj) || !_is_unicode_object(pref, vobj, py_v))
                            continue;
                        char* s = _string_remote(pref, vobj, py_v);
                        if (isvalid(s))
                            return s;
                    }
                    continue;
                }

            } else if (V_MIN(3, 12)) {
                // DictOrValues at obj[-3]: low bit tags dict (0) vs values (1).
                raddr_t dorv = NULL;
                copy_memory(pref, (char*)val_obj - 3 * (ssize_t)sizeof(raddr_t), sizeof(raddr_t), &dorv);
                if ((uintptr_t)dorv & 1) {
                    // CPython stores (values_ptr - 1) to tag bit 0; recover with + 1.
                    raddr_t vp = (raddr_t)((uintptr_t)dorv + 1);
                    // capacity is at vp[-1]; read it together with up to 32 value slots.
                    char    vbuf[1 + 32 * sizeof(raddr_t)];
                    if (fail(copy_memory(pref, (char*)vp - 1, sizeof(vbuf), vbuf)))
                        continue;
                    uint8_t cap = *(uint8_t*)vbuf;
                    if (cap > 32)
                        cap = 32;
                    raddr_t* slots = (raddr_t*)((char*)vbuf + 1);
                    for (uint8_t k = 0; k < cap; k++) {
                        raddr_t vobj = slots[k];
                        if (!isvalid(vobj) || !_is_unicode_object(pref, vobj, py_v))
                            continue;
                        char* s = _string_remote(pref, vobj, py_v);
                        if (isvalid(s))
                            return s;
                    }
                    continue;
                }
                inst_dict = dorv;

            } else { // 3.11
                // dict_ptr at obj[-3] and vp at obj[-4] are adjacent — read both at once.
                raddr_t pre[2] = {NULL, NULL};
                copy_memory(pref, (char*)val_obj - 4 * (ssize_t)sizeof(raddr_t), sizeof(pre), pre);
                raddr_t vp       = pre[0]; // obj[-4]
                raddr_t dict_ptr = pre[1]; // obj[-3]
                if (isvalid(dict_ptr)) {
                    inst_dict = dict_ptr;
                } else if (isvalid(vp)) {
                    // capacity at vp[-1]; read together with up to 32 slots.
                    char vbuf[1 + 32 * sizeof(raddr_t)];
                    if (fail(copy_memory(pref, (char*)vp - 1, sizeof(vbuf), vbuf)))
                        continue;
                    uint8_t cap = *(uint8_t*)vbuf;
                    if (cap > 32)
                        cap = 32;
                    raddr_t* slots = (raddr_t*)((char*)vbuf + 1);
                    for (uint8_t k = 0; k < cap; k++) {
                        raddr_t vobj = slots[k];
                        if (!isvalid(vobj) || !_is_unicode_object(pref, vobj, py_v))
                            continue;
                        char* s = _string_remote(pref, vobj, py_v);
                        if (isvalid(s))
                            return s;
                    }
                    continue;
                }
            }

        } else { // 3.10: traditional tp_dictoffset, split dict
            Py_ssize_t tp_dictoffset = 0;
            if (fail(copy_memory(pref, ob_type + _TP_DICTOFFSET_OFF(py_v), sizeof(tp_dictoffset), &tp_dictoffset)))
                continue;
            if (tp_dictoffset == 0)
                continue;
            copy_memory(pref, (char*)val_obj + tp_dictoffset, sizeof(raddr_t), &inst_dict);
            if (isvalid(inst_dict)) {
                ssize_t base  = py_v->py_object_size;
                // ma_keys and ma_values are adjacent in PyDictBody — read both at once.
                raddr_t kv[2] = {NULL, NULL};
                copy_memory(pref, inst_dict + base + (ssize_t)offsetof(PyDictBody, ma_keys), sizeof(kv), kv);
                raddr_t ma_keys   = kv[0];
                raddr_t ma_values = kv[1];
                if (isvalid(ma_values) && isvalid(ma_keys)) {
                    // Split dict: find "_name" in shared ma_keys, return ma_values[index].
                    // Read full keys header in one call.
                    PyDictKeysObject3_10 khdr;
                    if (fail(copy_memory(pref, ma_keys, sizeof(khdr), &khdr)))
                        continue;
                    ssize_t nslots   = khdr.dk_size;
                    ssize_t nentries = khdr.dk_nentries;
                    if (nslots > 0 && nslots <= (1 << 20) && nentries > 0 && nentries <= 65536) {
                        ssize_t ibs = (nslots <= 0xff) ? 1 : (nslots <= 0xffff) ? 2 : (nslots <= 0xffffffff) ? 4 : 8;
                        raddr_t entries = (raddr_t)((char*)ma_keys + sizeof(PyDictKeysObject3_10) + nslots * ibs);
                        for (ssize_t k = 0; k < nentries; k++) {
                            raddr_t eaddr   = (raddr_t)((char*)entries + k * sizeof(PyDictKeyEntry));
                            raddr_t key_obj = NULL;
                            copy_memory(pref, eaddr + offsetof(PyDictKeyEntry, me_key), sizeof(raddr_t), &key_obj);
                            if (!isvalid(key_obj))
                                continue;
                            char* ks = _string_remote(pref, key_obj, py_v);
                            if (!isvalid(ks))
                                continue;
                            int match = (strcmp(ks, "_name") == 0);
                            free(ks);
                            if (!match)
                                continue;
                            raddr_t name_obj = NULL;
                            copy_memory(pref, (char*)ma_values + k * sizeof(raddr_t), sizeof(raddr_t), &name_obj);
                            if (isvalid(name_obj))
                                return _string_remote(pref, name_obj, py_v);
                            break;
                        }
                    }
                    continue; // split dict handled
                }
            }
        }

        if (!isvalid(inst_dict))
            continue;

        // Regular combined __dict__: look up "_name" directly.
        raddr_t name_obj = py_dict__lookup_str(pref, inst_dict, "_name", py_v);
        if (!isvalid(name_obj))
            continue;

        return _string_remote(pref, name_obj, py_v);
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// Resolve threading._active raddr from interp (slow path).
// ----------------------------------------------------------------------------
static inline raddr_t
_resolve_active_dict(proc_ref_t pref, raddr_t interp_raddr, python_v* py_v) {
    raddr_t sys_modules = NULL;

    if (py_v->py_is.o_imports_modules) {
        // 3.10-3.11 and 3.13+: interp->modules IS sys.modules directly.
        if (fail(copy_memory(pref, interp_raddr + py_v->py_is.o_imports_modules, sizeof(raddr_t), &sys_modules)))
            return NULL;
    } else if (py_v->py_is.o_sysdict) {
        // 3.12: interp->sysdict is sys.__dict__; look up "modules" in it.
        raddr_t sysdict = NULL;
        if (fail(copy_memory(pref, interp_raddr + py_v->py_is.o_sysdict, sizeof(raddr_t), &sysdict)))
            return NULL;
        if (!isvalid(sysdict))
            return NULL;
        sys_modules = py_dict__lookup_str(pref, sysdict, "modules", py_v);
    } else {
        return NULL;
    }

    if (!isvalid(sys_modules))
        return NULL;

    raddr_t threading_mod = py_dict__lookup_str(pref, sys_modules, "threading", py_v);
    if (!isvalid(threading_mod))
        return NULL;

    // PyModuleObject.md_dict is the first field after PyObject_HEAD.
    raddr_t mod_dict = NULL;
    if (fail(copy_memory(pref, threading_mod + py_v->py_object_size, sizeof(raddr_t), &mod_dict)))
        return NULL;
    if (!isvalid(mod_dict))
        return NULL;

    return py_dict__lookup_str(pref, mod_dict, "_active", py_v);
}

// ----------------------------------------------------------------------------
// Count live threads in the interpreter by walking tstate_head; stops at 2
// since we only care whether there is exactly one thread.
// ----------------------------------------------------------------------------
static inline int
_count_threads_capped2(proc_ref_t pref, raddr_t interp_raddr, python_v* py_v) {
    raddr_t tstate = NULL;
    if (fail(copy_memory(pref, interp_raddr + py_v->py_is.o_tstate_head, sizeof(raddr_t), &tstate)))
        return 0;
    if (!isvalid(tstate))
        return 0;

    int     count = 1;
    raddr_t next  = NULL;
    if (fail(copy_memory(pref, tstate + py_v->py_thread.o_next, sizeof(raddr_t), &next)))
        return count;
    if (isvalid(next) && next != tstate)
        count++;
    return count;
}

// ----------------------------------------------------------------------------
// py_thread__resolve_name: attempt to fill entry->name and set entry->name_state.
// ----------------------------------------------------------------------------
static inline void
py_thread__resolve_name(
    proc_ref_t pref, raddr_t interp_raddr, interpreter_state_t* istate, thread_tracker_entry_t* entry, python_v* py_v
) {
    // Use native_id (= PyThreadState.thread_id = pthread_t on all platforms) for
    // threading._active ident matching.  On Linux, entry->tid is the kernel TID
    // (after pthread_tid_offset conversion) which does NOT match _active keys.
    long tid = (long)entry->native_id;

    // Fast path: use cached _active raddr.
    if (isvalid(istate->active_dict_raddr)) {
        char* name = _lookup_name_in_active(pref, istate->active_dict_raddr, tid, py_v);
        if (isvalid(name)) {
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            free(name);
            entry->name_state = NAME_RESOLVED;
            return;
        }
        // Read failed or tid absent — dict may have been reallocated or this is a
        // native thread not registered with threading.
        log_d("threading._active read failed or tid not found, re-resolving");
        istate->active_dict_raddr = NULL;
    }

    // Slow path: traverse sys.modules → threading → _active.
    raddr_t active = _resolve_active_dict(pref, interp_raddr, py_v);
    if (isvalid(active)) {
        istate->active_dict_raddr = active;
        char* name                = _lookup_name_in_active(pref, active, tid, py_v);
        if (isvalid(name)) {
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            free(name);
            entry->name_state = NAME_RESOLVED;
            return;
        }
        // threading imported but tid absent → native thread, use TID fallback.
        entry->name[0]    = '\0';
        entry->name_state = NAME_RESOLVED;
        return;
    }

    // threading not imported yet.
    int nthreads = _count_threads_capped2(pref, interp_raddr, py_v);
    if (nthreads == 1) {
        snprintf(entry->name, sizeof(entry->name), "MainThread");
        entry->name_state = NAME_RESOLVED;
    } else {
        // Retry next sample — threading may be imported later.
        entry->name_state = NAME_NO_THREADING;
    }
}
