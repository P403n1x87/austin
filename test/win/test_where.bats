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


function where_austin {
  local version="${1}"

  check_python $version

  log "Where [Python $version]"

  # -------------------------------------------------------------------------
  step "Where sleepy"
  # -------------------------------------------------------------------------
    $PYTHON test/sleepy.py &
    sleep 1
    run $AUSTIN -w $!

    assert_success
    assert_output "Process"
    assert_output "Thread"
    assert_output "sleepy.py"
    assert_output "<module>"
}


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test Where mode" {
  repeat 3 where_austin "3.9"
}
