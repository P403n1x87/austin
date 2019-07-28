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
#include "platform.h"
#include "version.h"


#define UNSUPPORTED_VERSION             log_w("Unsupported Python version detected. Austin might not work as expected.")

#define LATEST_VERSION                  &python_v3_7

#define PY_CODE(s) {                    \
  sizeof(s),                            \
  offsetof(s, co_filename),             \
  offsetof(s, co_name),                 \
  offsetof(s, co_lnotab),               \
  offsetof(s, co_firstlineno)           \
}

#define PY_FRAME(s) {                   \
  sizeof(s),                            \
  offsetof(s, f_back),                  \
  offsetof(s, f_code),                  \
  offsetof(s, f_lasti),                 \
}

/* Hack. Python 3.3 and below don't have the prev field */
#define PY_THREAD_H(s) {                \
  sizeof(s),                            \
  offsetof(s, next),                    \
  offsetof(s, next),                    \
  offsetof(s, interp),                  \
  offsetof(s, frame),                   \
  offsetof(s, thread_id)                \
}

#define PY_THREAD(s) {                  \
  sizeof(s),                            \
  offsetof(s, prev),                    \
  offsetof(s, next),                    \
  offsetof(s, interp),                  \
  offsetof(s, frame),                   \
  offsetof(s, thread_id)                \
}

#define PY_UNICODE(n) {                 \
  n                                     \
}

#define PY_BYTES(n) {                   \
  n                                     \
}

#define PY_RUNTIME(n) {                 \
  n                                     \
}

// ---- Python 2 --------------------------------------------------------------

python_v python_v2 = {
  PY_CODE     (PyCodeObject2),
  PY_FRAME    (PyFrameObject2),
  PY_THREAD_H (PyThreadState2),
  PY_UNICODE  (2),
  PY_BYTES    (2)
};

// ---- Python 3.3 ------------------------------------------------------------

python_v python_v3_3 = {
  PY_CODE     (PyCodeObject3_3),
  PY_FRAME    (PyFrameObject2),
  PY_THREAD_H (PyThreadState2),
  PY_UNICODE  (3),
  PY_BYTES    (3)
};

// ---- Python 3.4 ------------------------------------------------------------

python_v python_v3_4 = {
  PY_CODE     (PyCodeObject3_3),
  PY_FRAME    (PyFrameObject2),
  PY_THREAD   (PyThreadState3_4),
  PY_UNICODE  (3),
  PY_BYTES    (3)
};

// ---- Python 3.6 ------------------------------------------------------------

python_v python_v3_6 = {
  PY_CODE     (PyCodeObject3_6),
  PY_FRAME    (PyFrameObject2),
  PY_THREAD   (PyThreadState3_4),
  PY_UNICODE  (3),
  PY_BYTES    (3)
};

// ---- Python 3.7 ------------------------------------------------------------

python_v python_v3_7 = {
  PY_CODE     (PyCodeObject3_6),
  PY_FRAME    (PyFrameObject3_7),
  PY_THREAD   (PyThreadState3_4),
  PY_UNICODE  (3),
  PY_BYTES    (3),
  PY_RUNTIME  (0)
};


// ----------------------------------------------------------------------------
void
set_version(int version) {
  int minor = (version >> 8)  & 0xFF;
  int major = (version >> 16) & 0xFF;

  switch (major) {

  // ---- Python 2 ------------------------------------------------------------
  case 2:
    switch (minor) {
    case 0:
    case 1:
    case 2:
      UNSUPPORTED_VERSION;  // NOTE: These versions haven't been tested.

    // 2.3, 2.4, 2.5, 2.6, 2.7
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: py_v = &python_v2;
      break;

    default: py_v = &python_v2;
      UNSUPPORTED_VERSION;
    }
    break;

  // ---- Python 3 ------------------------------------------------------------
  case 3:
    switch (minor) {
    case 0:
    case 1:
    case 2:
      UNSUPPORTED_VERSION;  // NOTE: These versions haven't been tested.

    // 3.3
    case 3: py_v = &python_v3_3; break;

    // 3.4, 3.5
    case 4:
    case 5: py_v = &python_v3_4; break;

    // 3.6
    case 6: py_v = &python_v3_6; break;

    // 3.7
    case 7: py_v = &python_v3_7; break;

    default: py_v = LATEST_VERSION;
      UNSUPPORTED_VERSION;
    }
  }
}
