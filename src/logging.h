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

#pragma once

#ifdef TRACE
#define DEBUG
#endif

#include <inttypes.h>

#include "ansi.h"
#include "argparse.h"
#include "austin.h"

#if defined __clang__
#define COMPILER       "clang"
#define COMPILER_MAJOR __clang_major__
#define COMPILER_MINOR __clang_minor__
#define COMPILER_PATCH __clang_patchlevel__
#elif defined __MUSL__
#define COMPILER       "musl-gcc"
#define COMPILER_MAJOR __GNUC__
#define COMPILER_MINOR __GNUC_MINOR__
#define COMPILER_PATCH __GNUC_PATCHLEVEL__
#elif defined __GNUC__
#define COMPILER       "gcc"
#define COMPILER_MAJOR __GNUC__
#define COMPILER_MINOR __GNUC_MINOR__
#define COMPILER_PATCH __GNUC_PATCHLEVEL__
#elif defined _MSC_VER
#define COMPILER       "msvc"
#define COMPILER_MAJOR _MSC_VER / 100
#define COMPILER_MINOR _MSC_VER % 100
#define COMPILER_PATCH _MSC_BUILD
#endif

#define MICROSECONDS_FMT "%" PRIu64

#ifdef NATIVE
#define log_header()                                                                                         \
    {                                                                                                        \
        log_m(BOLD "              _   _      " CRESET);                                                      \
        log_m(BOLD " __ _ _  _ __| |_(_)_ _  " CRESET);                                                      \
        log_m(BOLD "/ _` | || (_-<  _| | ' \\ " CRESET);                                                     \
        log_m(                                                                                               \
            BOLD "\\__,_|\\_,_/__/\\__|_|_||_|" CRESET BRED "p" CRESET " " BCYN VERSION CRESET " [" COMPILER \
                 " %d.%d.%d]",                                                                               \
            COMPILER_MAJOR, COMPILER_MINOR, COMPILER_PATCH                                                   \
        );                                                                                                   \
        log_i("====[ AUSTINP ]====");                                                                        \
    }
#else
#define log_header()                                                                                       \
    {                                                                                                      \
        log_m(BOLD "              _   _      " CRESET);                                                    \
        log_m(BOLD " __ _ _  _ __| |_(_)_ _  " CRESET);                                                    \
        log_m(BOLD "/ _` | || (_-<  _| | ' \\ " CRESET);                                                   \
        log_m(                                                                                             \
            BOLD "\\__,_|\\_,_/__/\\__|_|_||_|" CRESET " " BCYN VERSION CRESET " [" COMPILER " %d.%d.%d]", \
            COMPILER_MAJOR, COMPILER_MINOR, COMPILER_PATCH                                                 \
        );                                                                                                 \
        log_i("====[ AUSTIN ]====");                                                                       \
    }
#endif
#define log_footer() \
    {}

/**
 * Initialise logger.
 *
 * This must be called before making any logging requests.
 */
void
logger_init(void);

/**
 * Log an entry at the various supported levels.
 */
void
log_f(const char*, ...);

void
log_e(const char*, ...);

void
log_w(const char*, ...);

void
log_i(const char*, ...);

void
log_m(const char*, ...); // metrics

#ifdef DEBUG
void
log_d(const char*, ...);
#else
#define log_d(f, args...) \
    {}
#endif

#ifdef TRACE
void
log_t(const char*, ...);
#else
#define log_t(f, args...) \
    {}
#endif

/**
 * Close the logger.
 *
 * This should be called as soon as the logger is no longer required.
 */
void
logger_close(void);

void
log_meta_header(void);
