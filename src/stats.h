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

#ifndef STATS_H
#define STATS_H


#include "argparse.h"


typedef unsigned long ctime_t;  /* Forward */
typedef unsigned long ustat_t;  /* non-negative statistics metric */


#ifndef STATS_C
extern unsigned long _sample_cnt;

extern ctime_t _min_sampling_time;
extern ctime_t _max_sampling_time;
extern ctime_t _avg_sampling_time;

extern ustat_t _error_cnt;
extern ustat_t _long_cnt;

extern ctime_t _gc_time;
#endif


/**
 * Get the current boot time in microseconds. This is intended to give
 * something that is as close as possible to wall-clock time.
 */
ctime_t
gettime(void);


/**
 * Reset the statistics. Call this every time a new run is started.
 */
void
stats_reset(void);


/**
 * Get the maximum sampling time observed.
 */
ctime_t
stats_get_max_sampling_time(void);


/**
 * Get the smallest sampling time observed.
 */
ctime_t
stats_get_min_sampling_time(void);


/**
 * Get the average sampling time from the last reset up to the moment this
 * method is called.
 */
ctime_t
stats_get_avg_sampling_time(void);


/**
 * Increase the sample counter.
 */
#define stats_count_sample()            { _sample_cnt++; }


/**
 * Increase the counter of samples with errors.
 */
#define stats_count_error()             { _error_cnt++; }


/**
 * Accumulate GC time.
 */
#define stats_gc_time(delta)             { _gc_time+=(delta); }


/**
 * Check the duration of the last sampling and update the statistics.
 *
 * @param ctime_t the time it took to obtain the sample.
 * @param ctime_t the sampling interval.
 */
#define stats_check_duration(delta) { \
  if (delta > pargs.t_sampling_interval)                 \
    _long_cnt++;                                         \
  if (_min_sampling_time > delta)                        \
    _min_sampling_time = delta;                          \
  else if (_max_sampling_time < delta)                   \
    _max_sampling_time = delta;                          \
  _avg_sampling_time += delta;                           \
}


/**
 * Log the current statistics. Usually called at the end of a sampling run.
 */
void
stats_log_metrics(void);


/**
 * Set the start time.
 */
void
stats_start(void);


/**
 * Return the current sampling duration.
 */
ctime_t
stats_duration(void);

#endif
