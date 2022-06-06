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
    metadata,
    processes,
    python,
    samples,
    sum_metric,
    target,
    threads,
)

from flaky import flaky


@flaky
@allpythons()
def test_pipe_wall_time(py):
    interval = 1
    result = austin("-Pi", f"{interval}ms", *python(py), target())
    assert result.returncode == 0

    meta = metadata(result.stdout)

    assert ".".join((str(_) for _ in meta["python"])).startswith(py), meta
    assert meta["mode"] == "wall", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == str(interval * 1000), meta

    assert len(processes(result.stdout)) == 1
    ts = threads(result.stdout)
    assert len(ts) == 2, ts

    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:32")
    assert not has_pattern(result.stdout, "Unwanted")

    a = sum_metric(result.stdout)
    d = int(meta["duration"])

    assert 0 < 0.8 * d < a < 2.2 * d


@allpythons()
def test_pipe_cpu_time(py):
    result = austin("-sPi", "1ms", *python(py), target())
    assert result.returncode == 0

    meta = metadata(result.stdout)

    assert ".".join((str(_) for _ in meta["python"])).startswith(py), meta
    assert meta["mode"] == "cpu", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == "1000", meta


@flaky(max_runs=3)
@allpythons()
def test_pipe_wall_time_multiprocess(py):
    result = austin("-CPi", "1ms", *python(py), target())
    assert result.returncode == 0

    meta = metadata(result.stdout)

    assert meta["mode"] == "wall", meta
    assert int(meta["duration"]) > 100000, meta
    assert meta["interval"] == "1000", meta
    assert meta["multiprocess"] == "on", meta
    assert ".".join((str(_) for _ in meta["python"])).startswith(py), meta


@flaky
@allpythons()
def test_pipe_wall_time_multiprocess_output(py, tmp_path):
    datafile = tmp_path / "test_pipe.austin"

    result = austin("-CPi", "1ms", "-o", str(datafile), *python(py), target())
    assert result.returncode == 0

    with datafile.open() as f:
        data = f.read()
        meta = metadata(data)

        assert meta, meta
        assert meta["mode"] == "wall", meta
        assert int(meta["duration"]) > 100000, meta
        assert meta["interval"] == "1000", meta
        assert meta["multiprocess"] == "on", meta
        assert ".".join((str(_) for _ in meta["python"])).startswith(py), meta

        assert has_pattern(data, "target34.py:keep_cpu_busy:32"), compress(data)

        a = sum(int(_.rpartition(" ")[-1]) for _ in samples(data))
        d = int(meta["duration"])

        assert 0 < 0.8 * d < a < 2.2 * d
