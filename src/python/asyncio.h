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
//
// COPYRIGHT NOTICE: The content of this file is composed of different parts
//                   taken from different versions of the source code of
//                   Python. The authors of those sources hold the copyright
//                   for most of the content of this header file.

// Debug offsets for asyncio introspection, exposed by the _asyncio extension
// module (Modules/_asynciomodule.c) since CPython 3.14. Unlike the main
// _Py_DebugOffsets structure, this one carries no cookie/version field and is
// not resolvable as a dynamic symbol: it lives in a section named
// "AsyncioDebug" within the _asyncio extension module image (a separate
// shared object, loaded lazily on `import asyncio`), so it must be located by
// scanning that image's sections rather than by symbol lookup. Its shape has
// been stable across 3.14 and 3.15; every field carries its own "size" so
// callers should never hardcode struct widths (they differ, e.g., on
// free-threaded builds where TaskObj gains a task_tid field).

#pragma once

#include <stdint.h>

typedef struct _Py_AsyncioModuleDebugOffsets {
    struct {
        uint64_t size;
        uint64_t task_name;
        uint64_t task_awaited_by;
        uint64_t task_is_task;
        uint64_t task_awaited_by_is_set;
        uint64_t task_coro;
        uint64_t task_node;
    } asyncio_task_object;

    struct {
        uint64_t size;
        uint64_t asyncio_tasks_head;
    } asyncio_interpreter_state;

    struct {
        uint64_t size;
        uint64_t asyncio_running_loop;
        uint64_t asyncio_running_task;
        uint64_t asyncio_tasks_head;
    } asyncio_thread_state;
} Py_AsyncioModuleDebugOffsets;
