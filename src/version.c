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

#define VERSION_C

#include "logging.h"
#include "version.h"


// ---- Python 3.4 ------------------------------------------------------------

python_v python_v3_4 = {
  // py_code
  {
    sizeof(PyCodeObject3_4),

    offsetof(PyCodeObject3_4, co_filename),
    offsetof(PyCodeObject3_4, co_name),
    offsetof(PyCodeObject3_4, co_lnotab),
    offsetof(PyCodeObject3_4, co_firstlineno)
  }
};


// ---- Python 3.6 ------------------------------------------------------------

python_v python_v3_6 = {
  {
    sizeof(PyCodeObject3_6),

    offsetof(PyCodeObject3_6, co_filename),
    offsetof(PyCodeObject3_6, co_name),
    offsetof(PyCodeObject3_6, co_lnotab),
    offsetof(PyCodeObject3_6, co_firstlineno)
  }
};


// ----------------------------------------------------------------------------
void
set_version(int version) {
  int minor = (version >> 8)  & 0xFF;
  int major = (version >> 16) & 0xFF;

  switch (major) {

  // ---- Python 2 ------------------------------------------------------------
  case 2:
    log_e("Python 2 is not supported yet.");
    break;

  // ---- Python 3 ------------------------------------------------------------
  case 3:
    switch (minor) {

    // NOTE: These versions haven't been tested.
    case 0:
    case 1:
    case 2:
    case 3:

    // 3.4, 3.5
    case 4:
    case 5:
      py_v = &python_v3_4;
      break;

    // 3.6, 3.7
    case 6:
    case 7:
      py_v = &python_v3_6;
      break;

    default:
      log_w("Unsupported Python 3 version detected. Austin might not work as expected.");
      py_v = &python_v3_6;
    }
  }
}
