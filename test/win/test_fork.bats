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

  log "Fork [Python $version]"

  # -------------------------------------------------------------------------
  step "Standard profiling"
  # -------------------------------------------------------------------------
    run $AUSTIN -i 1ms -t 1s $PYTHON test/target34.py

    assert_success
    assert_output "# austin: [[:digit:]]*.[[:digit:]]*.[[:digit:]]*"
    assert_output ".*test/target34.py:keep_cpu_busy:32"
    assert_not_output "Unwanted"

  # -------------------------------------------------------------------------
  step "Memory profiling"
  # -------------------------------------------------------------------------
    run $AUSTIN -i 1000 -t 1000 -m $PYTHON test/target34.py

    assert_success
    assert_output ".*test/target34.py:keep_cpu_busy:32"

  # -------------------------------------------------------------------------
  step "Output file"
  # -------------------------------------------------------------------------
    run $AUSTIN -i 10000 -t 1000 -o /tmp/austin_out.txt $PYTHON test/target34.py

    assert_success
    assert_output "Unwanted"
    assert_not_output ".*test/target34.py:keep_cpu_busy:32"
    assert_file "/tmp/austin_out.txt" ".*test/target34.py:keep_cpu_busy:32"

}

# -----------------------------------------------------------------------------

function teardown {
  if [ -f /tmp/austin_out.txt ]; then rm /tmp/austin_out.txt; fi
}


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test Austin with Python" {
  repeat 3 invoke_austin
}
