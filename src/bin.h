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

typedef unsigned int bin_attr_t;

#define BINARY_MIN_SIZE        (1 << 20)         // A meaningful Python binary takes MBs.

#define BINARY_TYPE(x)                  (x & 3)  // Get binary type
#define BT_OTHER                       0         // Other type of binary
#define BT_EXEC                        1         // Binary is executable
#define BT_LIB                         2         // Binary is shared library

#define B_SYMBOLS               (1 << 2)         // If the binary has symbols

#define B_BSS                   (1 << 3)         // If the BSS section was located
