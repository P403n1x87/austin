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


function invoke_austin {
  local version="${1}"

  check_python $version

  log "GC State Sampling [Python $version]"

  # -------------------------------------------------------------------------
  step "Standard profiling"
  # -------------------------------------------------------------------------
    run $AUSTIN -i 10ms -t 1s $PYTHON test/target_gc.py

    assert_success
    assert_not_output ":GC:"

  # -------------------------------------------------------------------------
  step "GC Sampling"
  # -------------------------------------------------------------------------
    run $AUSTIN -i 10ms -t 1s -g $PYTHON test/target_gc.py

    assert_success
    assert_output_min_occurrences 10 ":GC:"

  # -------------------------------------------------------------------------
  step "GC Sampling :: GC disabled"
  # -------------------------------------------------------------------------
    export GC_DISABLED=1
    run $AUSTIN -i 10ms -t 1s -g $PYTHON test/target_gc.py
    unset GC_DISABLED

    assert_success
    assert_output_max_occurrences 5 ":GC:"

}

# -----------------------------------------------------------------------------

function teardown {
  if [ -f /tmp/austin_out.txt ]; then rm /tmp/austin_out.txt; fi
}


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test GC Sampling with Python 3.7" {
  repeat 3 invoke_austin "3.7"
}

@test "Test GC Sampling with Python 3.8" {
  repeat 3 invoke_austin "3.8"
}

@test "Test GC Sampling with Python 3.9" {
  repeat 3 invoke_austin "3.9"
}

@test "Test GC Sampling with Python 3.10" {
  repeat 3 invoke_austin "3.10"
}
