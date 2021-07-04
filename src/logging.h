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


#define META "# "

#define NL fputc('\n', pargs.output_file)

#define meta(...)                          \
  fprintf(pargs.output_file, META);        \
  fprintf(pargs.output_file, __VA_ARGS__); \
  NL;

#define log_header() {                   \
  log_m("\033[1m              _   _      \033[0m");    \
  log_m("\033[1m __ _ _  _ __| |_(_)_ _  \033[0m");    \
  log_m("\033[1m/ _` | || (_-<  _| | ' \\ \033[0m");   \
  log_m("\033[1m\\__,_|\\_,_/__/\\__|_|_||_|\033[0m \033[36;1m%s\033[0m", VERSION); \
  log_i("====[ AUSTIN ]===="); \
}
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
