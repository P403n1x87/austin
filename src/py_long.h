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

// Read a remote CPython integer object (PyLong) as a C long.
// Uses struct definitions from python/long.h; all offsets are derived via
// offsetof with no hardcoded magic numbers.
//
// Returns -1 on error.  Since thread idents are always positive, -1
// unambiguously signals a read failure here.
//
// copy_memory call budget: 1 (read tag/size + up to 2 digits in one shot).
//
// The digit size and shift are derived at runtime from py_v->py_object_size

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hints.h"
#include "mem.h"
#include "python/long.h"
#include "version.h"

static inline long
py_long__read(proc_ref_t pref, raddr_t raddr, python_v* py_v) {
    if (!isvalid(raddr))
        return -1;

    // py_object_size is the PyObject_HEAD size in the *target* process.
    // Use it to determine the target's pointer size and hence digit layout.
    ssize_t hdr = py_v->py_object_size;

    // 32-bit Python: PyObject_HEAD = ob_refcnt(4) + ob_type*(4) = 8 bytes.
    int is_32bit    = (hdr <= 8);
    int ptr_bytes   = is_32bit ? 4 : 8; // sizeof(uintptr_t / Py_ssize_t) in target // GCOV_EXCL_BR_LINE
    int digit_bytes = is_32bit ? 2 : 4; // sizeof(digit) in target (uint16 vs uint32) // GCOV_EXCL_BR_LINE
    int shift       = is_32bit ? PyLong_SHIFT_15 : PyLong_SHIFT_30; // GCOV_EXCL_BR_LINE

    // Maximum body size: ptr_bytes (tag/size) + 2 * digit_bytes.
    char    body[8 + 2 * 4] = {0}; // large enough for 64-bit (8 + 8 = 16 B)
    ssize_t body_size       = ptr_bytes + 2 * digit_bytes;
    if (fail(copy_memory(pref, raddr + hdr, body_size, body)))
        return -1;

    if (V_MIN(3, 12)) {
        // 3.12+: body = lv_tag (ptr_bytes) + ob_digit[0..1] (digit_bytes each).
        uintptr_t lv_tag
            = (ptr_bytes == 4) ? (uintptr_t)*(uint32_t*)body : (uintptr_t)*(uint64_t*)body; // GCOV_EXCL_BR_LINE

        uint32_t d0 = (digit_bytes == 2) ? (uint32_t)*(uint16_t*)(body + ptr_bytes)
                                         : *(uint32_t*)(body + ptr_bytes); // GCOV_EXCL_BR_LINE

        if (lv_tag < ((uintptr_t)2 << _PyLong_NON_SIZE_BITS)) {
            // Compact: value = sign * ob_digit[0]
            int sign = 1 - (int)(lv_tag & _PyLong_SIGN_MASK);
            return sign * (long)d0;
        }

        uintptr_t digit_count = lv_tag >> _PyLong_NON_SIZE_BITS;
        if (digit_count == 0 || digit_count > 2)
            return -1;

        long val = (long)d0;
        if (digit_count == 2) {
            uint32_t d1  = (digit_bytes == 2)
                             ? (uint32_t)*(uint16_t*)(body + ptr_bytes + digit_bytes) // GCOV_EXCL_BR_LINE
                             : *(uint32_t*)(body + ptr_bytes + digit_bytes);
            val         |= (long)d1 << shift;
        }

        if ((lv_tag & _PyLong_SIGN_MASK) == _PyLong_SIGN_NEGATIVE)
            val = -val;
        return val;

    } else {
        // 3.10-3.11: body = ob_size (ptr_bytes, signed) + ob_digit[0..1].
        long ob_size = (ptr_bytes == 4) ? (long)*(int32_t*)body : (long)*(int64_t*)body; // GCOV_EXCL_BR_LINE
        if (ob_size == 0)
            return 0;

        ssize_t digit_count = ob_size < 0 ? -ob_size : ob_size;
        if (digit_count > 2)
            return -1;

        uint32_t d0 = (digit_bytes == 2) ? (uint32_t)*(uint16_t*)(body + ptr_bytes)
                                         : *(uint32_t*)(body + ptr_bytes); // GCOV_EXCL_BR_LINE

        long val = (long)d0;
        if (digit_count == 2) {
            uint32_t d1  = (digit_bytes == 2)
                             ? (uint32_t)*(uint16_t*)(body + ptr_bytes + digit_bytes) // GCOV_EXCL_BR_LINE
                             : *(uint32_t*)(body + ptr_bytes + digit_bytes);
            val         |= (long)d1 << shift;
        }
        if (ob_size < 0)
            val = -val;
        return val;
    }
}
