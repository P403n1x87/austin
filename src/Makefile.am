# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http:#www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2018 Gabriele N. Tornetta <phoenix1987@gmail.com>.
# All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http:#www.gnu.org/licenses/>.

AM_CFLAGS = -I$(srcdir) -Wall -Werror -Wno-unused-command-line-argument -pthread
OPT_FLAGS = -O3
STRIP_FLAGS = -Os -s
DEBUG_OPTS = 

if DEBUG_SYMBOLS
DEBUG_OPTS += -g
undefine STRIP_FLAGS
endif

if DEBUG
DEBUG_OPTS += -DDEBUG
austin_LDADD = -lm
endif

if COVERAGE
COVERAGE_FLAGS = -g -fprofile-arcs -ftest-coverage
undefine OPT_FLAGS
undefine STRIP_FLAGS
endif

man_MANS = austin.1

bin_PROGRAMS = austin

# ---- Austin ----

austin_CFLAGS = $(AM_CFLAGS) $(OPT_FLAGS) $(STRIP_FLAGS) $(COVERAGE_FLAGS) $(DEBUG_OPTS)
austin_SOURCES = \
  argparse.c     \
  austin.c       \
  cache.c        \
  env.c          \
  error.c        \
  events.c       \
  logging.c      \
  stack.c        \
  stats.c        \
  platform.c     \
  py_proc_list.c \
  py_proc.c      \
  py_thread.c


# ---- Austin P ----

if BUILD_AUSTINP
bin_PROGRAMS += austinp

austinp_SOURCES = $(austin_SOURCES)
austinp_CFLAGS = $(austin_CFLAGS) @AUSTINP_CFLAGS@
austinp_LDADD = $(austin_LDADD) @AUSTINP_LDADD@
endif
