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

#ifndef STRHASH_H
#define STRHASH_H

#include <stdlib.h>
#include <string.h>

#define MAGIC_TINY                            7
#define MAGIC_BIG                       1000003

// Stolen from stringobject.c
static inline long
string_hash(char * string) {
  register unsigned char *p;
  register long x;

  p = (unsigned char *) string;
  x = *p << MAGIC_TINY;
  while (*p != 0)
    x = (MAGIC_BIG * x) ^ *(p++);
  x ^= strlen(string);
  return x == 0 ? 1 : x;
}

#endif
