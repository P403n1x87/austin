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

variant "austinp"

test_case() {
  # TODO: austinp tests seem to be a bit flaky so we cap the duration of each
  # run for now.
  timeout --foreground --preserve-status -k 1s 5m bats test/test_$1.bats
}

@test "Test austinp variant: fork" {
  test_case fork
}

@test "Test austinp variant: attach" {
  requires_root
  
  test_case attach
}

@test "Test austinp variant: valgrind" {
  ignore

  test_case valgrind
}

@test "Test austinp variant: kernel stacks" {
  requires_root
  
  test_case kernel
}
