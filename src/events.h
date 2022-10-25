// This file is part of "austin" which is released under GPL.
//
// See file LICENCE or go to http://www.gnu.org/licenses/ for full license
// details.
//
// Austin is a Python frame stack sampler for CPython.
//
// Copyright (c) 2018-2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

#ifndef EVENTS_H
#define EVENTS_H

#include <stdio.h>

#include "argparse.h"
#include "hints.h"
#include "logging.h"
#include "mojo.h"
#include "platform.h"

#if defined PL_WIN
#define fprintfp _fprintf_p
#else
#define fprintfp fprintf
#endif

#if defined PL_WIN
#define MEM_METRIC "%lld"
#elif defined __arm__
#define MEM_METRIC "%d"
#else
#define MEM_METRIC "%ld"
#endif
#define TIME_METRIC "%lu"
#define IDLE_METRIC "%d"
#define METRIC_SEP ","

#define emit_metadata(label, ...)        \
  {                                      \
    if (pargs.binary) {                  \
      mojo_metadata(label, __VA_ARGS__); \
    } else {                             \
      meta(label, __VA_ARGS__);          \
    }                                    \
  }

#define emit_invalid_frame()                    \
  {                                             \
    if (pargs.binary) {                         \
      mojo_event(MOJO_FRAME_INVALID);           \
    } else {                                    \
      fprintf(pargs.output_file, ";:INVALID:"); \
    }                                           \
  }

#define emit_gc()                          \
  {                                        \
    if (pargs.binary) {                    \
      mojo_event(MOJO_GC);                 \
    } else {                               \
      fprintf(pargs.output_file, ";:GC:"); \
    }                                      \
  }

#define emit_stack(format, pid, tid, ...)                         \
  {                                                               \
    if (pargs.binary) {                                           \
      mojo_stack(pid, tid);                                       \
    } else {                                                      \
      fprintfp(pargs.output_file, format, pid, tid, __VA_ARGS__); \
    }                                                             \
  }

#define emit_frame_ref(format, frame)                                      \
  {                                                                        \
    if (pargs.binary) {                                                    \
      mojo_frame_ref(frame);                                               \
    } else {                                                               \
      fprintfp(pargs.output_file, format, frame->filename,                 \
               frame->scope == UNKNOWN_SCOPE ? "<unknown>" : frame->scope, \
               frame->line);                                               \
    }                                                                      \
  }

#define emit_time_metric(value)                                \
  {                                                            \
    if (pargs.binary) {                                        \
      mojo_metric_time(value);                                 \
    } else {                                                   \
      fprintf(pargs.output_file, " " TIME_METRIC "\n", value); \
    }                                                          \
  }

#define emit_memory_metric(value)                             \
  {                                                           \
    if (pargs.binary) {                                       \
      mojo_metric_memory(value);                              \
    } else {                                                  \
      fprintf(pargs.output_file, " " MEM_METRIC "\n", value); \
    }                                                         \
  }

#define emit_full_metrics(time, idle, memory)                                          \
  {                                                                                    \
    if (pargs.binary) {                                                                \
      mojo_metric_time(time);                                                          \
      if (idle) {                                                                      \
        mojo_event(MOJO_IDLE);                                                         \
      }                                                                                \
      mojo_metric_memory(memory);                                                      \
    } else {                                                                           \
      fprintf(pargs.output_file,                                                       \
              " " TIME_METRIC METRIC_SEP IDLE_METRIC METRIC_SEP MEM_METRIC "\n", time, \
              idle, memory);                                                           \
    }                                                                                  \
  }

#ifdef NATIVE

#define emit_kernel_frame(format, scope)          \
  {                                               \
    if (pargs.binary) {                           \
      mojo_frame_kernel(scope);                   \
    } else {                                      \
      fprintfp(pargs.output_file, format, scope); \
    }                                             \
  }

#endif  // NATIVE

#ifdef DEBUG

#define emit_frames_left(n)                                \
  {                                                        \
    if (!pargs.binary) {                                   \
      fprintf(pargs.output_file, ";:%ld FRAMES LEFT:", n); \
    }                                                      \
  }

#endif  // DEBUG

#endif  // EVENTS_H
