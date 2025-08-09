// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#include <stdio.h>
#include <stdlib.h>

#include "env.h"
#include "hints.h"

// Globals for command line arguments
parsed_env_t env = {
    /* logging       */ true,
    /* page_size_cap */ 4096, // 4 KiB
};

// ----------------------------------------------------------------------------
static inline void
_env_error(char* var) {
    fprintf(stderr, "Invalid value '%s' for Austin environment variable '%s'\n", getenv(var), var);
}

// ----------------------------------------------------------------------------
static inline bool
_is_set(const char* s) {
    const char* v = getenv(s);
    return v != NULL && *v != '\0';
}

// ----------------------------------------------------------------------------
static inline int
_to_number(char* var, long* num, long default_value) {
    char* value = getenv(var);
    if (!isvalid(value)) {
        *num = default_value;
        return 0;
    }

    char* p_err;

    *num = strtol(value, &p_err, 10);

    return (p_err == value || *p_err != '\0') ? 1 : 0;
}

// ----------------------------------------------------------------------------
int
parse_env() {
    // AUSTIN_NO_LOGGING
    if (_is_set("AUSTIN_NO_LOGGING")) {
        env.logging = false;
    }

    // AUSTIN_PAGE_SIZE_CAP
    long page_size_cap;
    if (fail(_to_number("AUSTIN_PAGE_SIZE_CAP", &page_size_cap, env.page_size_cap))) {
        _env_error("AUSTIN_PAGE_SIZE_CAP");
        set_error(ENV, "Invalid page size cap");
        FAIL;
    }
    env.page_size_cap = (size_t)page_size_cap;

    SUCCESS;
}
