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

load "../common"


function where_austin {
  python_bin="${1}"

  if ! $python_bin -V; then skip "$python_bin not found."; fi

  log "Where [Python $python_bin]"

  # -------------------------------------------------------------------------
  step "Where sleepy"
  # -------------------------------------------------------------------------
    $python_bin test/sleepy.py &
    sleep 1
    run sudo $AUSTIN -w $!

    assert_success
    assert_output "Process"
    assert_output "Thread"
    assert_output "test/sleepy.py"
    assert_output "<module>"

}


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test Austin with default Python 3 from Homebrew" {
	where_austin "/usr/local/bin/python3"
}

@test "Test Austin with Python 3.8 from Homebrew" {
  repeat 3 where_austin "/usr/local/opt/python@3.8/bin/python3"
}

@test "Test Austin with Python 3.9 from Homebrew" {
  repeat 3 where_austin "/usr/local/opt/python@3.9/bin/python3"
}

@test "Test Austin with Python 3.10 from Homebrew" {
  repeat 3 where_austin "/usr/local/opt/python@3.10/bin/python3"
}

@test "Test Austin with Python 3 from Anaconda (if available)" {
  ignore
  repeat 3 where_austin "/usr/local/anaconda3/bin/python"
}
