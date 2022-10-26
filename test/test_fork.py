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

import platform
from pathlib import Path
from test.utils import (
    allpythons,
    austin,
    compress,
    demojo,
    has_pattern,
    maps,
    metadata,
    mojo,
    processes,
    python,
    samples,
    sum_metric,
    sum_metrics,
    target,
    threads,
    variants,
)

import pytest
from flaky import flaky


@flaky(max_runs=10)
@pytest.mark.parametrize("heap", [tuple(), ("-h", "0"), ("-h", "64")])
@allpythons()
@variants
@mojo
def test_fork_wall_time(austin, py, heap, mojo):
    result = austin("-i", "2ms", *heap, *python(py), target("target34.py"), mojo=mojo)
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.stdout)) == 1, compress(result.stdout)
    ts = threads(result.stdout)
    assert len(ts) == 2, compress(result.stdout)

    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:3"), compress(
        result.stdout
    )
    assert not has_pattern(result.stdout, "Unwanted")

    meta = metadata(result.stdout)

    assert meta["mode"] == "wall"

    a = sum_metric(result.stdout)
    d = int(meta["duration"])

    assert 0 < a < 2.1 * d

    if austin == "austinp":
        ms = maps(result.stdout)
        assert len(ms) >= 2, ms
        assert [_ for _ in ms if "python" in _], ms


@flaky
@pytest.mark.parametrize("heap", [tuple(), ("-h", "0"), ("-h", "64")])
@allpythons()
@variants
@mojo
def test_fork_cpu_time_cpu_bound(py, heap, austin, mojo):
    result = austin("-si", "1ms", *heap, *python(py), target("target34.py"), mojo=mojo)
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:3"), compress(
        result.stdout
    )
    assert not has_pattern(result.stdout, "Unwanted")

    meta = metadata(result.stdout)

    assert meta["mode"] == "cpu"

    a = sum_metric(result.stdout)
    d = int(meta["duration"])

    assert 0 < a < 2.1 * d


@flaky(max_runs=6)
@allpythons()
@variants
def test_fork_cpu_time_idle(py, austin):
    result = austin("-si", "1ms", *python(py), target("sleepy.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_pattern(result.stdout, "sleepy.py:<module>:"), compress(result.stdout)

    meta = metadata(result.stdout)

    a = sum_metric(result.stdout)
    d = int(meta["duration"])

    assert a < 1.1 * d


@flaky
@allpythons()
@mojo
def test_fork_memory(py, mojo):
    result = austin("-mi", "1ms", *python(py), target("target34.py"), mojo=mojo)
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:32")

    meta = metadata(result.stdout)

    assert meta["mode"] == "memory"

    d = int(meta["duration"])
    assert d > 100000

    ms = [int(_.rpartition(" ")[-1]) for _ in samples(result.stdout)]
    alloc = sum(_ for _ in ms if _ > 0)
    dealloc = sum(-_ for _ in ms if _ < 0)

    assert alloc * dealloc


@allpythons()
@mojo
def test_fork_output(py, tmp_path: Path, mojo):
    datafile = tmp_path / "test_fork_output.austin"

    result = austin(
        "-i", "1ms", "-o", str(datafile), *python(py), target("target34.py"), mojo=mojo
    )
    assert result.returncode == 0, result.stderr or result.stdout

    assert "Unwanted" in result.stdout

    data = demojo(datafile.read_bytes()) if mojo else datafile.read_text()

    assert has_pattern(data, "target34.py:keep_cpu_busy:32")

    meta = metadata(data)

    assert meta["mode"] == "wall"

    a = sum(int(_.rpartition(" ")[-1]) for _ in samples(data))
    d = int(meta["duration"])

    assert 0 < 0.9 * d < a < 2.1 * d


# Support for multiprocess is attach-like and seems to suffer from the same
# issues as attach tests on Windows.
@flaky
@pytest.mark.xfail(platform.system() == "Windows", reason="Does not pass in Windows CI")
@allpythons(min=(3, 7) if platform.system() == "Windows" else None)
@mojo
def test_fork_multiprocess(py, mojo):
    result = austin("-Ci", "1ms", *python(py), target("target_mp.py"), mojo=mojo)
    assert result.returncode == 0, result.stderr or result.stdout

    ps = processes(result.stdout)
    assert len(ps) >= 3, ps

    meta = metadata(result.stdout)
    assert meta["multiprocess"] == "on", meta
    assert meta["mode"] == "wall", meta

    assert has_pattern(result.stdout, "target_mp.py:do:"), compress(result.stdout)
    assert has_pattern(result.stdout, "target_mp.py:fact:31 "), compress(result.stdout)


@flaky
@allpythons()
@mojo
def test_fork_full_metrics(py, mojo):
    result = austin("-i", "10ms", "-f", *python(py), target("target34.py"), mojo=mojo)
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.stdout)) == 1
    ts = threads(result.stdout)
    assert len(ts) == 2, ts

    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:32")
    assert not has_pattern(result.stdout, "Unwanted")

    meta = metadata(result.stdout)

    assert meta["mode"] == "full"

    wall, cpu, alloc, dealloc = sum_metrics(result.stdout)
    d = int(meta["duration"])

    assert 0 < 0.9 * d < wall < 2.1 * d
    assert 0 < cpu <= wall
    assert alloc * dealloc


@flaky
@pytest.mark.parametrize("exposure", [1, 2])
@allpythons()
def test_fork_exposure(py, exposure):
    result = austin(
        "-i", "1ms", "-x", str(exposure), *python(py), target("sleepy.py"), "1"
    )
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_pattern(result.stdout, "sleepy.py:<module>:"), compress(result.stdout)

    meta = metadata(result.stdout)

    assert meta["mode"] == "wall"

    d = int(meta["duration"])
    assert 900000 * exposure < d < 1100000 * exposure


@variants
@allpythons(min=(3, 11))
def test_qualnames(py, austin):
    result = austin("-i", "1ms", *python(py), target("qualnames.py"))
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.stdout)) == 1, compress(result.stdout)
    ts = threads(result.stdout)
    assert len(ts) == 1, compress(result.stdout)

    assert has_pattern(result.stdout, "qualnames.py:Foo.run"), compress(result.stdout)
    assert has_pattern(result.stdout, "qualnames.py:Bar.run"), compress(result.stdout)


@allpythons()
def test_no_logging(py, monkeypatch):
    monkeypatch.setenv("AUSTIN_NO_LOGGING", "1")
    result = austin("-i", "1ms", *python(py), target("target34.py"))
    assert has_pattern(result.stdout, "target34.py:keep_cpu_busy:3"), compress(
        result.stdout
    )
    assert result.returncode == 0, result.stderr or result.stdout
