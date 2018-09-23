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
#include <syslog.h>

#include "austin.h"
#include "logging.h"


void
logger_init(void) {
  setlogmask (LOG_UPTO (LOG_DEBUG));
  openlog ("austin", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
}


void
log_e(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  vsyslog(LOG_ERR, fmt, args);

  va_end(args);
}

void
log_d(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  vsyslog(LOG_DEBUG, fmt, args);

  va_end(args);
}

void
log_w(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  vsyslog(LOG_WARNING, fmt, args);

  va_end(args);
}

void
log_i(const char * fmt, ...) {
  va_list args;
  va_start(args, fmt);

  vsyslog(LOG_INFO, fmt, args);

  va_end(args);
}


void log_version(void) {
  log_i("%s version: %s", PROGRAM_NAME, VERSION);
}


void
logger_close(void) {
  closelog();
}
