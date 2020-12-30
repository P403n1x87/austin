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

#ifndef TIMER_H
#define TIMER_H


#include <unistd.h>

#include "argparse.h"
#include "error.h"
#include "stats.h"


static ctime_t _sample_timestamp;
static ctime_t _sample_delta;


static inline void
timer_start(void) {
  _sample_timestamp = gettime();
  error = EOK;
} /* timer_start */


static inline ctime_t
timer_stop(void) {
  _sample_delta = gettime() - _sample_timestamp;

  // Record stats
  stats_check_duration(_sample_delta, pargs.t_sampling_interval);
  stats_count_sample();
  if (error != EOK)
    stats_count_error();

  return _sample_delta;
} /* timer_stop */


static inline void
timer_pause(ctime_t delta) {
  // Pause if sampling took less than the sampling interval.
  if (delta < pargs.t_sampling_interval)
    usleep(pargs.t_sampling_interval - delta);
}

#endif
