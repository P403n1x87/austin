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

from itertools import takewhile
from subprocess import check_output
from test.utils import allpythons
from test.utils import austin
from test.utils import has_frame
from test.utils import processes
from test.utils import python
from test.utils import sum_metrics
from test.utils import target
from test.utils import threads


@allpythons()
def test_pipe_wall_time(py):
    interval = 1
    result = austin("-Pi", f"{interval}ms", *python(py), target())
    assert result.returncode == 0

    meta = result.metadata

    assert meta["python"].startswith(py), meta
    assert meta["mode"] == "wall", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == str(interval * 1000), meta

    assert len(processes(result.samples)) == 1
    ts = threads(result.samples)
    assert len(ts) == 2, ts

    assert has_frame(result.samples, "target34.py", "keep_cpu_busy", 32)
    assert b"Unwanted" not in result.stdout

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])

    assert 0 < 0.8 * d < a < 2.2 * d


@allpythons()
def test_pipe_cpu_time(py):
    result = austin("-cPi", "1ms", *python(py), target())
    assert result.returncode == 0

    meta = result.metadata

    assert meta["python"].startswith(py), meta
    assert meta["mode"] == "cpu", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == "1000", meta


@allpythons()
def test_pipe_wall_time_multiprocess(py):
    result = austin("-CPi", "1ms", *python(py), target())
    assert result.returncode == 0

    meta = result.metadata

    assert meta["mode"] == "wall", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == "1000", meta
    assert meta["multiprocess"] == "on", meta
    assert meta["python"].startswith(py), meta


@allpythons()
def test_pipe_wall_time_multiprocess_output(py, tmp_path):
    result = austin("-CPi", "1ms", *python(py), target(), convert=True)
    assert result.returncode == 0

    meta = result.metadata

    assert meta, result.stdout
    assert meta["mode"] == "wall", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == "1000", meta
    assert meta["multiprocess"] == "on", meta
    assert meta["python"].startswith(py), meta


@allpythons(min=(3, 11))
def test_python_version(py):
    result = austin("-Pi", "1s", *python(py), target())
    assert result.returncode == 0

    meta = result.metadata

    reported_version = meta["python"]

    # Take the version from the interpreter itself, discarding anything after
    # the patchlevel.
    actual_version = "".join(
        takewhile(
            lambda _: _.isdigit() or _ == ".",
            (check_output([*python(py), "-V"]).decode().strip().partition(" ")[-1]),
        )
    )

    assert reported_version == actual_version, meta


# TODO: Test TTY behaviour
