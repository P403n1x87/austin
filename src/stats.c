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

#define STATS_C

#include "platform.h"

#include <limits.h>
#include <time.h>

#if defined PL_MACOS
#include <mach/clock.h>
#include <mach/mach.h>
#elif defined PL_WIN
#include <profileapi.h>
#endif

#include "error.h"
#include "logging.h"
#include "stats.h"


#ifndef CLOCK_BOOTTIME
  #ifdef CLOCK_REALTIME
    #define CLOCK_BOOTTIME CLOCK_REALTIME
  #else
    #define CLOCK_BOOTTIME HIGHRES_CLOCK
  #endif
#endif

// ---- PRIVATE ---------------------------------------------------------------

unsigned long _sample_cnt;

ctime_t _min_sampling_time;
ctime_t _max_sampling_time;
ctime_t _avg_sampling_time;

ustat_t _error_cnt;
ustat_t _long_cnt;

#if defined PL_WIN
// On Windows we have to use the QueryPerformance APIs in order to get the
// right time resolution. We use this variable to cache the inverse frequency
// (counts per second), that is the period of each count, in units of μs.
static double _period;
#endif


// ---- PUBLIC ----------------------------------------------------------------

ctime_t
gettime(void) {
  #if defined PL_UNIX                                                 /* UNIX */
  struct timespec ts;

  #ifdef PL_MACOS
  clock_serv_t cclock;
  mach_timespec_t mts;

  host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
  clock_get_time(cclock, &mts);
  mach_port_deallocate(mach_task_self(), cclock);

  ts.tv_sec = mts.tv_sec;
  ts.tv_nsec = mts.tv_nsec;

  #else
  clock_gettime(CLOCK_BOOTTIME, &ts);
  #endif

  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;

  #else                                                                /* WIN */
  LARGE_INTEGER count;
  QueryPerformanceCounter(&count);

  return count.QuadPart * _period;
  #endif
}


void
stats_reset(void) {
  _sample_cnt = 0;
  _error_cnt  = 0;

  _min_sampling_time = ULONG_MAX;
  _max_sampling_time = 0;
  _avg_sampling_time = 0;

  #if defined PL_WIN
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  _period = ((double) 1e6) / ((double) freq.QuadPart);
  #endif
}


ctime_t
stats_get_max_sampling_time(void) {
  return _max_sampling_time;
}


ctime_t
stats_get_min_sampling_time(void) {
  return _min_sampling_time;
}


ctime_t
stats_get_avg_sampling_time(void) {
  return _avg_sampling_time / _sample_cnt;
}


void
stats_log_metrics(void) {
  if (!_sample_cnt) {
    log_m("😣 No samples collected.");
    return;
  }

  log_m("🕑 Sampling time (min/avg/max) : %lu/%lu/%lu μs",
    stats_get_min_sampling_time(),
    stats_get_avg_sampling_time(),
    stats_get_max_sampling_time()
  );

  log_m("🐢 Long sampling rate : %d/%d (%.2f %%) samples took longer than the sampling interval", \
    _long_cnt,                                           \
    _sample_cnt,                                         \
    (float) _long_cnt / _sample_cnt * 100                \
  );

  log_m("💀 Error rate : %d/%d (%.2f %%) invalid samples",   \
    _error_cnt,                                          \
    _sample_cnt,                                         \
    (float) _error_cnt / _sample_cnt * 100               \
  );
}
