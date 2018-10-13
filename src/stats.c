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

#include <limits.h>
#include <time.h>

#include "error.h"
#include "logging.h"
#include "stats.h"


#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_REALTIME
#endif

// ---- PRIVATE ---------------------------------------------------------------

static unsigned long _sample_cnt;

static ctime_t _min_sampling_time;
static ctime_t _max_sampling_time;
static ctime_t _avg_sampling_time;

static ustat_t _error_cnt;
static ustat_t _long_cnt;


// ---- PUBLIC ----------------------------------------------------------------

ctime_t
gettime(void) {
  struct timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}


void
stats_reset(void) {
  _sample_cnt = 0;
  _error_cnt  = 0;

  _min_sampling_time = ULONG_MAX;
  _max_sampling_time = 0;
  _avg_sampling_time = 0;
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
stats_check_error(void) {
  _sample_cnt++;

  if (error != EOK)
    _error_cnt++;
}


void
stats_check_duration(ctime_t delta, ctime_t sampling_interval) {
  // Long-running samples
  if (delta > sampling_interval)
    _long_cnt++;

  // Max/min/avg sampling duration
  if (_min_sampling_time > delta)
    _min_sampling_time = delta;
  else if (_max_sampling_time < delta)
    _max_sampling_time = delta;

  _avg_sampling_time += delta;
}


void
stats_log_metrics(void) {
  log_i("Max/min/avg sampling time (in usecs): %lu/%lu/%lu",
    stats_get_max_sampling_time(),
    stats_get_min_sampling_time(),
    stats_get_avg_sampling_time()
  );

  log_i("Long-running sample rate: %d samples over sampling interval/%d (%.2f %%)\n", \
    _long_cnt,                                           \
    _sample_cnt,                                         \
    (float) _long_cnt / _sample_cnt * 100                \
  );

  log_i("Error rate: %d invalid samples/%d (%.2f %%)\n", \
    _error_cnt,                                          \
    _sample_cnt,                                         \
    (float) _error_cnt / _sample_cnt * 100               \
  );
}
