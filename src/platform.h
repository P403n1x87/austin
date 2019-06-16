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


#if defined(__linux__)
  #define PL_LINUX

#elif defined(__APPLE__) && defined(__MACH__)
  #define PL_MACOS

#elif defined(_WIN32) || defined(_WIN64)
  #define PL_WIN

  #define NULL_DEVICE "NUL:"

#endif

// ----------------------------------------------------------------------------

#if defined(PL_LINUX) || defined(PL_MACOS)
  #define PL_UNIX

  #define NULL_DEVICE "/dev/null"
#endif

#endif
