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

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>


#if defined(__linux__)
  #define PL_LINUX
  #define _GNU_SOURCE

  #include <sys/types.h>

  typedef pid_t proc_ref_t;

#elif defined(__APPLE__) && defined(__MACH__)
  #define PL_MACOS

  #include <mach/mach_port.h>

  typedef mach_port_t proc_ref_t;

#elif defined(_WIN32) || defined(_WIN64)
  #define PL_WIN

  #include <windows.h>

  #define NULL_DEVICE "NUL:"

  typedef HANDLE proc_ref_t;

#endif

// ----------------------------------------------------------------------------

#if defined(AUSTINP) && defined(PL_LINUX)
#define NATIVE
#endif

// ----------------------------------------------------------------------------

#if defined(PL_LINUX) || defined(PL_MACOS)
  #define PL_UNIX

  #define NULL_DEVICE "/dev/null"
#endif

// ----------------------------------------------------------------------------

#if defined PL_MACOS
#define PID_MAX                    99999  // From sys/proc_internal.h
#endif


/**
 * Get the maximum PID for the platform.
 */
size_t
pid_max();

#endif