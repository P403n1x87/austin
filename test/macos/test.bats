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


test_case() {
  run bats test/macos/test_$1.bats
}


@test "Test Austin: fork" {
  test_case fork
}

@test "Test Austin: fork multi-process" {
  test_case fork_mp
}

@test "Test Austin: attach" {
  test_case attach
}

@test "Test Austin: valgrind" {
  ignore
  if ! which valgrind; then skip "Valgrind not found"; fi
  test_case valgrind
}
