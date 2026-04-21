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

#include "platform.h"

#if defined(PL_WIN)
#include <windows.h>
#else
#include <unistd.h>
#endif

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
// Precise sleep — sub-ms on Windows via high-resolution waitable timer, falling
// back to usleep elsewhere.  The Windows default timer tick (~15.6ms) otherwise
// causes usleep(1000) to routinely take ~15ms, inflating the sampling period
// by an order of magnitude.

#if defined(PL_WIN)
// Per-process high-resolution timer (Win10 1803+).  The flag may be absent in
// older mingw headers, so we redefine it defensively.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Undocumented NTDLL entry point (already linked via -lntdll).  Drops the
// system timer tick from ~15.6ms to 0.5ms so the Sleep() fallback path has
// usable precision on systems where the waitable timer cannot be created.
__declspec(dllimport) LONG __stdcall
NtSetTimerResolution(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

#ifndef AUSTIN_C
extern
#endif
    HANDLE _pacer_timer;
#ifndef AUSTIN_C
extern
#endif
    int _pacer_resolution_raised;
#endif

static inline void
_precise_sleep(microseconds_t us) {
#if defined(PL_WIN)
    if (_pacer_timer != NULL) {
        LARGE_INTEGER due;
        // Negative = relative time in 100-ns units.
        due.QuadPart = -(LONGLONG)(us * 10);
        if (SetWaitableTimer(_pacer_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(_pacer_timer, INFINITE);
            return;
        }
    }
    // Last-resort fallback (pre-Win10-1803 or handle creation failure).
    // Without a high-resolution source, Sleep is bound by the ~15.6ms tick.
    Sleep((DWORD)((us + 999) / 1000));
#else
    usleep((useconds_t)us);
#endif
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
#if defined(PL_WIN)
    if (_pacer_timer == NULL) {
        _pacer_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (_pacer_timer == NULL) {
            // Best-effort: ask the kernel for the finest tick available (0.5ms)
            // so that the Sleep() fallback is not bound by the 15.6ms default.
            ULONG current = 0;
            if (NtSetTimerResolution(5000, TRUE, &current) == 0)
                _pacer_resolution_raised = 1;
        }
    }
#endif
    p->deadline = gettime() + pargs.t_sampling_interval;
}

// Sleep until the next sample is due.  Returns the actual sleep duration in
// microseconds, or 0 when the sampler is saturated (behind schedule).
static inline microseconds_t
pacer_next(pacer_t* p) {
    if (pargs_native) {
        // Sleep the full interval — sampling time was dead time for the target.
        _precise_sleep(pargs.t_sampling_interval);
        return pargs.t_sampling_interval;
    }

    microseconds_t now = gettime();

    if (p->deadline > now) {
        microseconds_t delay = p->deadline - now;
        _precise_sleep(delay);
        p->deadline += pargs.t_sampling_interval;
        return delay;
    }
    p->deadline += pargs.t_sampling_interval;
    return 0;
}

static inline void
pacer_free(void) {
#if defined(PL_WIN)
    if (_pacer_timer != NULL) {
        CloseHandle(_pacer_timer);
        _pacer_timer = NULL;
    }
    if (_pacer_resolution_raised) {
        ULONG current = 0;
        (void)NtSetTimerResolution(0, FALSE, &current);
        _pacer_resolution_raised = 0;
    }
#endif
}
