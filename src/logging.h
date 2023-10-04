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

#ifndef LOGGING_H
#define LOGGING_H

#ifdef TRACE
#define DEBUG
#endif

#include "argparse.h"
#include "austin.h"


#define META_HEAD "# "
#define META_SEP  ": "

#define NL {if (!pargs.binary) fputc('\n', pargs.output_file);}

#define meta(key, ...)                     \
  fputs(META_HEAD, pargs.output_file);     \
  fputs(key, pargs.output_file);           \
  fputs(META_SEP, pargs.output_file);      \
  fprintf(pargs.output_file, __VA_ARGS__); \
  NL;

#if defined __clang__
#define COMPILER "clang"
#define COMPILER_MAJOR __clang_major__
#define COMPILER_MINOR __clang_minor__
#define COMPILER_PATCH __clang_patchlevel__
#elif defined __MUSL__
#define COMPILER "musl-gcc"
#define COMPILER_MAJOR __GNUC__
#define COMPILER_MINOR __GNUC_MINOR__
#define COMPILER_PATCH __GNUC_PATCHLEVEL__
#elif defined __GNUC__
#define COMPILER "gcc"
#define COMPILER_MAJOR __GNUC__
#define COMPILER_MINOR __GNUC_MINOR__
#define COMPILER_PATCH __GNUC_PATCHLEVEL__
#elif defined _MSC_VER
#define COMPILER "msvc"
#define COMPILER_MAJOR _MSC_VER / 100
#define COMPILER_MINOR _MSC_VER % 100
#define COMPILER_PATCH _MSC_BUILD
#endif

#ifdef NATIVE
#define log_header() {                   \
  log_m("\033[1m              _   _      \033[0m");    \
  log_m("\033[1m __ _ _  _ __| |_(_)_ _  \033[0m");    \
  log_m("\033[1m/ _` | || (_-<  _| | ' \\ \033[0m");   \
  log_m("\033[1m\\__,_|\\_,_/__/\\__|_|_||_|\033[0m\033[31;1mp\033[0m \033[36;1m" VERSION "\033[0m [" COMPILER " %d.%d.%d]", COMPILER_MAJOR, COMPILER_MINOR, COMPILER_PATCH); \
  log_i("====[ AUSTINP ]===="); \
}
#else
#define log_header() {                   \
  log_m("\033[1m              _   _      \033[0m ");    \
  log_m("\033[1m __ _ _  _ __| |_(_)_ _  \033[0m");    \
  log_m("\033[1m/ _` | || (_-<  _| | ' \\ \033[0m");   \
  log_m("\033[1m\\__,_|\\_,_/__/\\__|_|_||_|\033[0m \033[36;1m" VERSION "\033[0m [" COMPILER " %d.%d.%d]", COMPILER_MAJOR, COMPILER_MINOR, COMPILER_PATCH); \
  log_i("====[ AUSTIN ]===="); \
}
#endif
#define log_footer() {}

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
log_f(const char *, ...);

void
log_e(const char *, ...);

void
log_w(const char *, ...);

void
log_i(const char *, ...);

void
log_m(const char *, ...);  // metrics


/**
 * Log indirect error.
 *
 * Messages logged this way are prepended with a symbol that indicates that
 * they are a consequence of the error right above. A root cause is then an
 * unprefixed error/fatal log entry.
 */
#define log_ie(msg) log_e("> " msg)


#ifdef DEBUG
void
log_d(const char *, ...);
#else
#define log_d(f, args...) {}
#endif

#ifdef TRACE
void
log_t(const char *, ...);
#else
#define log_t(f, args...) {}
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

#endif // LOGGING_H
