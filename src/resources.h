// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#pragma once

#include "hints.h"
#include "platform.h"

#define CLEANUP_TYPE(type, func)         \
    static inline void func##t(type **v) \
    {                                    \
        if (isvalid(*v))                 \
        {                                \
            func(*v);                    \
            *v = NULL;                   \
        }                                \
    }                                    \
    struct __allow_semicolon__

#define CLEANUP_FUNC_SENTINEL(type, func, sentinel) \
    static inline void func##type(type *v)          \
    {                                               \
        if (*v != sentinel)                         \
        {                                           \
            func(*v);                               \
            *v = sentinel;                          \
        }                                           \
    }                                               \
    struct __allow_semicolon__

#define CLEANUP_FUNC(type, func)            \
    static inline void func##type(type **v) \
    {                                       \
        if (isvalid(*v))                    \
        {                                   \
            func(*v);                       \
            *v = NULL;                      \
        }                                   \
    }                                       \
    struct __allow_semicolon__

CLEANUP_FUNC(void, free);
#define cu_void __attribute__((cleanup(freevoid))) void

CLEANUP_FUNC(char, free);
#define cu_char __attribute__((cleanup(freechar))) char

typedef unsigned char uchar;
CLEANUP_FUNC(uchar, free);
#define cu_uchar __attribute__((cleanup(freeuchar))) uchar

CLEANUP_FUNC(int, free);
#define cu_int __attribute__((cleanup(freeint))) int

CLEANUP_FUNC(FILE, fclose);
#define cu_FILE __attribute__((cleanup(fcloseFILE))) FILE

// ---- Unix ----

#if defined PL_UNIX
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct
{
    void *addr;
    size_t size;
} map_t;

#define _popen popen
#define _pclose pclose

static inline map_t *
map_new(int fd, size_t size, int flags)
{
    void *addr = mmap(0, size, PROT_READ, flags, fd, 0);
    if (!isvalid(addr))
        return NULL;

    map_t *map = malloc(sizeof(map_t));
    if (map == MAP_FAILED)
    {
        munmap(map, size);
        return NULL;
    }

    map->size = size;
    map->addr = addr;

    return map;
}

static inline void
map__destroy(map_t *map)
{
    if (isvalid(map))
    {
        munmap(map->addr, map->size);
        free(map);
    }
}

CLEANUP_FUNC(map_t, map__destroy);
#define cu_map_t __attribute__((cleanup(map__destroymap_t))) map_t

CLEANUP_FUNC_SENTINEL(int, close, -1);
#define cu_fd __attribute__((cleanup(closeint))) int

#endif // PL_UNIX

CLEANUP_FUNC(FILE, _pclose);
#define cu_pipe __attribute__((cleanup(_pcloseFILE))) FILE

// ---- Linux resources ----

#if defined PL_LINUX
#include <dirent.h>

CLEANUP_FUNC(DIR, closedir);
#define cu_DIR __attribute__((cleanup(closedirDIR))) DIR

#endif // PL_LINUX

// ---- Windows resources ----

#if defined PL_WIN
CLEANUP_FUNC_SENTINEL(HANDLE, CloseHandle, INVALID_HANDLE_VALUE);
#define cu_HANDLE __attribute__((cleanup(CloseHandleHANDLE))) HANDLE
#endif // PL_WIN
