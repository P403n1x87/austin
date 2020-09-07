# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2019 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

load "common"


function attach_austin {
  local version="${1}"

  check_python $version

  log "Attach [Python $version]"

  # -------------------------------------------------------------------------
  step "Standard profiling"
  # -------------------------------------------------------------------------
    $PYTHON test/sleepy.py &
    sleep 1
    run $AUSTIN -i 100 -t 100 -p $!

    assert_success
    assert_output "(test/sleepy.py);L[[:digit:]]* "

}


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test Austin with Python 2.3" {
  ignore
	repeat 3 attach_austin "2.3"
}

@test "Test Austin with Python 2.4" {
  ignore
	repeat 3 attach_austin "2.4"
}

@test "Test Austin with Python 2.5" {
	repeat 3 attach_austin "2.5"
}

@test "Test Austin with Python 2.6" {
	repeat 3 attach_austin "2.6"
}

@test "Test Austin with Python 2.7" {
	repeat 3 attach_austin "2.7"
}

@test "Test Austin with Python 3.3" {
	repeat 3 attach_austin "3.3"
}

@test "Test Austin with Python 3.4" {
	repeat 3 attach_austin "3.4"
}

@test "Test Austin with Python 3.5" {
	repeat 3 attach_austin "3.5"
}

@test "Test Austin with Python 3.6" {
  repeat 3 attach_austin "3.6"
}

@test "Test Austin with Python 3.7" {
  repeat 3 attach_austin "3.7"
}

@test "Test Austin with Python 3.8" {
  repeat 3 attach_austin "3.8"
}
