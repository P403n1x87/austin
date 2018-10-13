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

#include <stdarg.h>

#if defined(__linux__)
#include <syslog.h>

#elif defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <stdio.h>

#define	LOG_EMERG	0	/* system is unusable */
#define	LOG_ALERT	1	/* action must be taken immediately */
#define	LOG_CRIT	2	/* critical conditions */
#define	LOG_ERR		3	/* error conditions */
#define	LOG_WARNING	4	/* warning conditions */
#define	LOG_NOTICE	5	/* normal but significant condition */
#define	LOG_INFO	6	/* informational */
#define	LOG_DEBUG	7	/* debug-level messages */

FILE * lf = NULL;
#endif

#include "austin.h"
#include "logging.h"


void
_log_writer(int prio, const char * fmt, va_list ap) {
  #if defined(__linux__)
  vsyslog(prio, fmt, ap);

  #elif defined(_WIN32) || defined(_WIN64)
  if (lf == NULL) {
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
  }
  else {
    vfprintf(lf, fmt, ap); fputc('\n', lf);
    fflush(lf);
  }

  #endif
}


void
logger_init(void) {
  #if defined(__linux__)
  setlogmask (LOG_UPTO (LOG_DEBUG));
  openlog ("austin", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);

  #elif defined(_WIN32) || defined(_WIN64)
  if (lf == NULL) {
    char path[MAX_PATH];
    ExpandEnvironmentStrings("%TEMP%\\austin.log", path, MAX_PATH);
    lf = fopen(path, "a");
  }
  #endif
}


void
log_e(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_ERR, fmt, args);

  va_end(args);
}

void
log_d(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  _log_writer(LOG_DEBUG, fmt, args);

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


void log_version(void) {
  log_i("%s version: %s", PROGRAM_NAME, VERSION);
}


void
logger_close(void) {
  #if defined(__linux__)
  closelog();

  #elif defined(_WIN32) || defined(_WIN64)
  if (lf != NULL)
    fclose(lf);
  #endif
}
