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
#include <stdint.h>
#include <time.h>

#if defined PL_MACOS
#include <mach/clock.h>
#include <mach/mach.h>
#elif defined PL_WIN
#include <profileapi.h>
#endif

#include "argparse.h"
#include "error.h"
#include "events.h"
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

microseconds_t _min_sampling_time;
microseconds_t _max_sampling_time;
microseconds_t _avg_sampling_time;

microseconds_t _start_time;

ustat_t _sample_cnt;
ustat_t _error_cnt;
ustat_t _long_cnt;

microseconds_t _gc_time;

#if defined PL_MACOS
static clock_serv_t cclock;
#elif defined PL_WIN
// On Windows we have to use the QueryPerformance APIs in order to get the
// right time resolution. We use this variable to cache the inverse frequency
// (counts per second), that is the period of each count, in units of μs.
static long long _period;
#endif

// ---- PUBLIC ----------------------------------------------------------------

microseconds_t
gettime() {
#ifdef PL_MACOS
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;

#elif defined PL_LINUX
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * ((microseconds_t)1000000) + ts.tv_nsec / 1000;

#else /* WIN */
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return count.QuadPart * 1000000 / _period;
#endif
}

void
stats_reset() {
    _sample_cnt = 0;
    _error_cnt  = 0;

    _min_sampling_time = MICROSECONDS_MAX;
    _max_sampling_time = 0;
    _avg_sampling_time = 0;

#if defined PL_MACOS
    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
#elif defined PL_WIN
    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq) == 0) {
        log_e("Failed to get frequency count");
    }
    _period = freq.QuadPart;
#endif
}

microseconds_t
stats_get_max_sampling_time() {
    return _max_sampling_time;
}

microseconds_t
stats_get_min_sampling_time() {
    return _min_sampling_time;
}

microseconds_t
stats_get_avg_sampling_time() {
    return _avg_sampling_time / _sample_cnt;
}

void
stats_start() {
    _start_time = gettime();
}

microseconds_t
stats_duration() {
    return gettime() - _start_time;
}

#define STAT_INDENT "      "

void
stats_log_metrics() {
    microseconds_t duration = stats_duration();

    event_handler__emit_metadata("count", "%ld", _sample_cnt);
    event_handler__emit_metadata("duration", MICROSECONDS_FMT, duration);

    if (_sample_cnt) {
        event_handler__emit_metadata(
            "sampling", MICROSECONDS_FMT "," MICROSECONDS_FMT "," MICROSECONDS_FMT, stats_get_min_sampling_time(),
            stats_get_avg_sampling_time(), stats_get_max_sampling_time()
        );
        event_handler__emit_metadata("saturation", "%ld/%ld", _long_cnt, _sample_cnt);
        event_handler__emit_metadata("errors", "%ld/%ld", _error_cnt, _sample_cnt);
        if (pargs.gc)
            event_handler__emit_metadata("gc", MICROSECONDS_FMT, _gc_time);

        if (pargs.pipe)
            goto release; // Saves a few computations

        log_m("");
        log_m("📈 " BOLD "Sampling Statistics" CRESET);
        log_m("");

        log_m(STAT_INDENT "Total duration" BLK " . . . . . . " CRESET BOLD "%.2fs" CRESET, duration / 1000000.);

        double      avg_rate   = (double)_sample_cnt / (duration / 1000000.);
        const char* rate_unit  = "Hz";
        double      rate_value = avg_rate;
        if (avg_rate >= 1e6) { // GCOV_EXCL_START
            rate_unit  = "MHz";
            rate_value = avg_rate / 1e6;
        } else if (avg_rate >= 1e3) { // GCOV_EXCL_STOP
            rate_unit  = "kHz";
            rate_value = avg_rate / 1e3;
        }
        log_m(STAT_INDENT "Average sampling rate" BLK "  . . " CRESET BOLD "%.2f %s" CRESET, rate_value, rate_unit);

        if (pargs.gc) {
            log_m(
                STAT_INDENT "Garbage collector" BLK "  . . . . " CRESET BOLD "%.2fs" CRESET " (" BOLD "%.2f%%" CRESET
                            ")",
                _gc_time / 1000000., (float)_gc_time / duration * 100
            );
        }

        log_m(
            STAT_INDENT "Error rate" BLK " . . . . . . . . " CRESET BOLD "%d/%d" CRESET " (" BOLD "%.2f%%" CRESET ")",
            _error_cnt, _sample_cnt, (float)_error_cnt / _sample_cnt * 100
        );
    } else {
        log_m("");
        log_m("😣 No samples collected.");
    }

release:
#if defined PL_MACOS
    mach_port_deallocate(mach_task_self(), cclock);
#endif
    return;
}
