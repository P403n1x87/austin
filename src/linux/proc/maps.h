// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2021 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../error.h"
#include "../../resources.h"
#include "../common.h"

// ----------------------------------------------------------------------------

typedef struct _proc_map {
    void*   address;
    size_t  size;
    uint8_t perms;
    char*   pathname;

    struct _proc_map* next;
} proc_map_t;

#define PERMS_READ  (1 << 0)
#define PERMS_WRITE (1 << 1)
#define PERMS_EXEC  (1 << 2)

#define PROC_MAP_ITER(proc_maps, map) for (proc_map_t* map = proc_maps; isvalid(map); map = map->next)

// ----------------------------------------------------------------------------
static inline proc_map_t*
proc_map_new(pid_t pid) {
    cu_char*    line           = NULL;
    size_t      len            = 0;
    char        pathname[1024] = {0};
    char        perms[5]       = {0};
    proc_map_t* head           = NULL;
    proc_map_t* curr           = NULL;
    proc_map_t* next           = NULL;

    cu_FILE* fp = _procfs(pid, "maps");
    if (!isvalid(fp)) { // GCOV_EXCL_START
        switch (errno) {
        case EACCES: // Needs elevated privileges
            set_error(PERM, "Cannot read from procfs");
            break;
        case ENOENT: // Invalid pid
            set_error(OS, "No such process");
            break;
        default:
            set_error(OS, "Unknown error");
        }
        FAIL_PTR;
    } // GCOV_EXCL_STOP

    while (getline(&line, &len, fp) != -1) {
        ssize_t lower, upper;

        int has_pathname = sscanf(
                               line, "%zx-%zx %s %*x %*x:%*x %*x %s\n", &lower,
                               &upper,  // Map bounds
                               perms,   // Permissions
                               pathname // Binary path
                           )
                         - 3; // We expect between 3 and 4 matches. We skip offset, dev and inode

        if (has_pathname < 0) {
            // Too few columns. This shouldn't happen but we skip this case
            // anyway.
            continue;
        }
        if (has_pathname && pathname[0] == '[') {
            // Skip kernel memory maps
            continue;
        }

        next = (proc_map_t*)calloc(1, sizeof(proc_map_t));
        if (!isvalid(next)) { // GCOV_EXCL_START
            set_error(MALLOC, "Cannot allocate memory for proc_map_t");
            FAIL_PTR;
        } // GCOV_EXCL_STOP
        if (!isvalid(head))
            head = next;
        else
            curr->next = next;

        curr = next;

        curr->address   = (void*)lower;
        curr->size      = upper - lower;
        curr->perms    |= PERMS_READ * (perms[0] == 'r');
        curr->perms    |= PERMS_WRITE * (perms[1] == 'w');
        curr->perms    |= PERMS_EXEC * (perms[2] == 'x');
        curr->pathname  = has_pathname ? strdup(pathname) : NULL;
    }

    if (!isvalid(head)) {
        set_error(OS, "No memory maps found");
        FAIL_PTR;
    }

    return head;
}

// ----------------------------------------------------------------------------
static inline proc_map_t*
proc_map__first(proc_map_t* self, char* pathname) {
    if (!isvalid(self) || !isvalid(pathname)) { // GCOV_EXCL_START
        set_error(NULL, "Invalid arguments to proc_map__first");
        return NULL;
    } // GCOV_EXCL_STOP

    PROC_MAP_ITER(self, map) {
        if (isvalid(map->pathname) && strcmp(map->pathname, pathname) == 0)
            return map;
    }

    set_error(OS, "No matching memory map found");
    return NULL;
}

// ----------------------------------------------------------------------------
static inline proc_map_t*
proc_map__first_submatch(proc_map_t* self, char* needle) {
    if (!isvalid(self) || !isvalid(needle)) // GCOV_EXCL_LINE
        return NULL;                        // GCOV_EXCL_LINE

    PROC_MAP_ITER(self, map) {
        if (isvalid(map->pathname) && isvalid(strstr(map->pathname, needle))) {
            return map;
        }
    }

    return NULL;
}

// ----------------------------------------------------------------------------
static inline void
proc_map__destroy(proc_map_t* self) {
    if (!isvalid(self))
        return;

    proc_map__destroy(self->next);

    sfree(self->pathname);

    free(self);
}

CLEANUP_TYPE(proc_map_t, proc_map__destroy);
#define cu_proc_map_t __attribute__((cleanup(proc_map__destroyt))) proc_map_t
