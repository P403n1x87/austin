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


#define log_header() log_i("============================  AUSTIN  ╦̵̵̿╤─ ҉ ~ •  ============================")
#define log_footer() log_i("============================================================================")


void
logger_init(void);


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

void
log_version(void);

void
logger_close(void);

#endif // LOGGING_H
