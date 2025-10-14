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

from test.utils import allpythons
from test.utils import austin
from test.utils import has_frame
from test.utils import python
from test.utils import target


@allpythons()
def test_accuracy_fast_recursive(py):
    result = austin("-i", "1ms", "-P", *python(py), target("recursive.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, "recursive.py", "sum_up_to")

    for sample in result.samples:
        if (
            not sample.frames
            or len(sample.frames) < 2
            or sample.frames[1].function != "sum_up_to"
        ):
            continue
        if len(sample.frames) > 20:
            raise AssertionError("recursive stack is not taller than actual recursion")
