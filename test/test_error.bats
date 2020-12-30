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


# -----------------------------------------------------------------------------
# -- Test Cases
# -----------------------------------------------------------------------------

@test "Test no arguments" {
  log "Test Austin with no arguments"

  run src/austin

  assert_success
  assert_output "Usage:"
}

@test "Test no command & PID" {
  log "Test Austin with no command nor PID"

  run src/austin -C

  assert_status 255
  assert_output "command to run or a PID"
}

@test "Test not Python" {
  log "Test Austin with a non-Python command"

  run src/austin cat

  assert_status 32             || assert_status 33
  assert_output "not a Python" || assert_output "Cannot launch"

  run src/austin -p 1

  assert_status 32
  assert_output "not a Python"
}

@test "Test invalid command" {
  log "Test Austin with an invalid command"

  run src/austin snafubar

  assert_status 33
  assert_output "Cannot launch"
}

@test "Test invalid PID" {
  log "Test Austin with an invalid PID"

  run src/austin -p 9999999

  assert_status 36
  assert_output "Cannot attach"
}

@test "Test no permission" {
  log "Test Austin with no permissions"

  if [[ $EUID -eq 0 ]]; then
    skip "must not be root"
  fi

  python3 test/sleepy.py &
  sleep 1
  run src/austin -i 100ms -p $!

  assert_status 37
  assert_output "Insufficient permissions"
}
