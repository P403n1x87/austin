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

#pragma once

#include <unistd.h>

#include "argparse.h"
#include "error.h"
#include "stats.h"

#ifndef AUSTIN_C
extern
#endif
    microseconds_t _sample_timer_start;

static inline void
sample_timer_start(void) {
    _sample_timer_start = gettime();
}

static inline microseconds_t
sample_timer_elapsed(void) {
    return gettime() - _sample_timer_start;
}

// ----------------------------------------------------------------------------
// Pacer — controls the sleep between sampling ticks.
//
// Non-native mode (deadline-based): threads run during sampling, so the
// sampling work counts towards the interval. A deadline is advanced by the
// interval each tick; we sleep only the remaining time. This is
// self-correcting: if one tick overruns, subsequent sleeps shorten to
// compensate.
//
// Native mode (fixed-sleep): threads are suspended during sampling, so the
// sampling duration is dead time for the target. After resuming we sleep
// the full interval to give threads the requested running time.

typedef struct {
    microseconds_t deadline;
} pacer_t;

static inline void
pacer_init(pacer_t* p) {
    p->deadline = gettime() + pargs.t_sampling_interval;
}

// Sleep until the next sample is due.  Returns the actual sleep duration in
// microseconds, or 0 when the sampler is saturated (behind schedule).
static inline microseconds_t
pacer_next(pacer_t* p) {
    if (pargs_native) {
        // Sleep the full interval — sampling time was dead time for the target.
        usleep((useconds_t)pargs.t_sampling_interval);
        return pargs.t_sampling_interval;
    }

    microseconds_t now = gettime();

    if (p->deadline > now) {
        microseconds_t delay = p->deadline - now;
        usleep((useconds_t)delay);
        p->deadline += pargs.t_sampling_interval;
        return delay;
    }
    p->deadline += pargs.t_sampling_interval;
    return 0;
}
