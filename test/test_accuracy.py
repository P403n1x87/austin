# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

from test.utils import (
    allpythons,
    austin,
    compress,
    has_pattern,
    python,
    samples,
    target,
)

import pytest
from flaky import flaky


@flaky
@pytest.mark.parametrize("heap", [tuple(), ("-h", "0"), ("-h", "64")])
@allpythons()
def test_accuracy_fast_recursive(py, heap):
    result = austin("-i", "1ms", *heap, *python(py), target("recursive.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_pattern(result.stdout, "sum_up_to"), compress(result.stdout)
    assert has_pattern(result.stdout, ":INVALID:"), compress(result.stdout)

    for _ in samples(result.stdout):
        if "sum_up_to" in _ and "<module>" in _:
            assert len(_.split(";")) <= 20, _
