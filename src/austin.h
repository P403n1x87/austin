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

#ifndef AUSTIN_H
#define AUSTIN_H

#include "platform.h"

#ifdef NATIVE
#define PROGRAM_NAME "austinp"
#else
#define PROGRAM_NAME "austin"
#endif
/* [[[cog
from scripts.utils import get_current_version_from_changelog as version
print(f'#define VERSION                         "{version()}"')
]]] */
#define VERSION                         "3.7.0"
// [[[end]]]

#endif
