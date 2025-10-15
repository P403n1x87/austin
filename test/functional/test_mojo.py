# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2023 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

from pathlib import Path
from test.utils import allpythons
from test.utils import austin
from test.utils import python
from test.utils import target

from austin.events import AustinSample
from austin.format.mojo import MojoStreamReader


@allpythons(min=(3, 11))
def test_mojo_column_data(py, tmp_path: Path):
    datafile = tmp_path / "test_mojo_column.austin"

    result = austin("-i", "100", "-o", str(datafile), *python(py), target("column.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    def strip(f):
        return (f.function, f.line, f.line_end, f.column, f.column_end)

    with datafile.open("rb") as f:
        frames = {
            strip(frame)
            for e in (
                _
                for _ in MojoStreamReader(f)
                if isinstance(_, AustinSample) and _.frames
            )
            for frame in e.frames
            if Path(frame.filename).name == "column.py"
        }

        assert frames & {
            ("<module>", 16, 19, 5, 2),
            ("<listcomp>", 16, 19, 5, 2),
            ("lazy", 6, 6, 9, 19),
            ("<listcomp>", 17, 17, 5, 17),
        }


@allpythons(max=(3, 10))
def test_mojo_no_column_data(py, tmp_path: Path):
    """
    Test that no other location information is present apart from the line
    number for Python versions prior to 3.11.
    """
    datafile = tmp_path / "test_mojo_column.austin"

    result = austin("-i", "100", "-o", str(datafile), *python(py), target("column.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    with datafile.open("rb") as f:
        assert {
            (frame.line_end, frame.column, frame.column_end)
            for e in (
                _
                for _ in MojoStreamReader(f)
                if isinstance(_, AustinSample) and _.frames
            )
            for frame in e.frames
            if isinstance(e, AustinSample)
        } == {(0, 0, 0)}
