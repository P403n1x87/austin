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

#define _DEFAULT_SOURCE

#include "events.h"
#include "mem.h"
#include "platform.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef PL_UNIX
#include <syslog.h>

#else
#include <windows.h>
#include <stdio.h>

#define LOG_EMERG   0  /* system is unusable */
#define LOG_ALERT   1  /* action must be taken immediately */
#define LOG_CRIT    2  /* critical conditions */
#define LOG_ERR     3  /* error conditions */
#define LOG_WARNING 4  /* warning conditions */
#define LOG_NOTICE  5  /* normal but significant condition */
#define LOG_INFO    6  /* informational */
#define LOG_DEBUG   7  /* debug-level messages */

FILE * logfile = NULL;
#endif

#include "austin.h"
#include "logging.h"

int logging = 1;

void
_log_writer(int prio, const char * fmt, va_list ap) {
  if (!logging) return;
  #ifdef PL_UNIX
  vsyslog(prio, fmt, ap);

  #else
  if (logfile == NULL) {
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    fflush(stderr);
  }
  else {
    vfprintf(logfile, fmt, ap); fputc('\n', logfile);
    fflush(logfile);
  }

  #endif
}

static int
has_nonempty_env(const char * s) {
  const char * v = getenv(s);
  return v != NULL && *v != '\0';
}

void
logger_init(void) {
  if (has_nonempty_env("AUSTIN_NO_LOGGING")) logging = 0;
  if (!logging) return;
  #ifdef PL_UNIX
  setlogmask (LOG_UPTO (LOG_DEBUG));
  openlog ("austin", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

  #else
  if (logfile == NULL) {
    char path[MAX_PATH];
    ExpandEnvironmentStrings("%TEMP%\\austin.log", path, MAX_PATH);
    logfile = fopen(path, "a");
  }
  #endif
}


void
log_f(const char * fmt, ...) {
  va_list args;

  va_start(args, fmt);
    _log_writer(LOG_CRIT, fmt, args);
  va_end(args);
}

void
log_e(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_ERR, fmt, args);

  va_end(args);
}

void
log_w(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_WARNING, fmt, args);

  va_end(args);
}

void
log_i(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_INFO, fmt, args);

  va_end(args);
}

void
log_m(const char * fmt, ...) {
  va_list args;

  va_start(args, fmt);
    vfprintf(stderr, fmt, args); fputc('\n', stderr);
    fflush(stderr);
  va_end(args);
}

#ifdef DEBUG
void
log_d(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_DEBUG, fmt, args);

  va_end(args);
}
#endif

#ifdef TRACE
void
log_t(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_DEBUG, fmt, args);

  va_end(args);
}
#endif


void
logger_close(void) {
  if (!logging) return;
  #ifdef PL_UNIX
  closelog();

  #else
  if (logfile != NULL)
    fclose(logfile);
  #endif
}

#if defined PL_WIN
#define MEM_VALUE "%llu"
#elif defined __arm__
#define MEM_VALUE "%u"
#else
#define MEM_VALUE "%lu"
#endif

void
log_meta_header(void) {
  emit_metadata("austin", VERSION);
  emit_metadata("interval", "%lu", pargs.t_sampling_interval);

  if (pargs.full)           { emit_metadata("mode", "full"); }
  else if (pargs.memory)    { emit_metadata("mode", "memory"); }
  else if (pargs.sleepless) { emit_metadata("mode", "cpu"); }
  else                      { emit_metadata("mode", "wall"); }

  if (pargs.memory || pargs.full) { emit_metadata("memory", MEM_VALUE, get_total_memory()); }
  if (pargs.children) { emit_metadata("multiprocess", "on"); }
}
