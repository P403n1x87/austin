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

import os
import platform
import signal
import sys
from pathlib import Path
from test.utils import allpythons
from test.utils import austin
from test.utils import parse_mojo
from test.utils import has_frame
from test.utils import maps
from test.utils import processes
from test.utils import python
from test.utils import sum_full_metrics
from test.utils import sum_metrics
from test.utils import target
from test.utils import threads
from test.utils import variants
from time import sleep

import pytest


@allpythons()
@variants
def test_fork_wall_time(austin, py):
    result = austin("-i", "2ms", *python(py), target("target34.py"))
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.samples)) == 1
    ts = threads(result.samples)
    assert len(ts) == 2

    assert has_frame(
        result.samples, filename="target34.py", function="keep_cpu_busy", line=32
    )
    assert b"Unwanted" not in result.stdout

    meta = result.metadata

    assert meta["mode"] == "wall"

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])

    assert 0 < a < 2.1 * d

    if austin == "austinp":
        ms = maps(result.stdout)
        assert len(ms) >= 2, ms
        assert [_ for _ in ms if "python" in _], ms


@allpythons()
@variants
def test_fork_cpu_time_cpu_bound(py, austin):
    result = austin("-ci", "1ms", *python(py), target("target34.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(
        result.samples, filename="target34.py", function="keep_cpu_busy", line=32
    )
    assert b"Unwanted" not in result.stdout

    meta = result.metadata

    assert meta["mode"] == "cpu"

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])

    assert 0 < a < 2.1 * d


@allpythons()
@variants
def test_fork_cpu_time_idle(py, austin):
    result = austin("-ci", "1ms", *python(py), target("sleepy.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, filename="sleepy.py", function="<module>")

    meta = result.metadata

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])

    assert a < 1.1 * d


@pytest.mark.parametrize("args", ("-m", "-cm"))
@allpythons()
def test_fork_memory(py, args):
    result = austin(args, "-i", "1ms", *python(py), target("target34.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, "target34.py", "keep_cpu_busy", 32)

    meta = result.metadata

    assert meta["mode"] == "memory"

    d = int(meta["duration"])
    assert d > 100000

    ms = [_.metrics.memory for _ in result.samples]
    alloc = sum(_ for _ in ms if _ > 0)
    dealloc = sum(-_ for _ in ms if _ < 0)

    assert alloc * dealloc


@allpythons()
def test_fork_output(py, tmp_path: Path):
    datafile = tmp_path / "test_fork_output.austin"

    result = austin(
        "-i", "1ms", "-o", str(datafile), *python(py), target("target34.py")
    )
    assert result.returncode == 0, result.stderr or result.stdout

    assert "Unwanted" in result.stdout

    samples, meta = parse_mojo(datafile.read_bytes())

    assert has_frame(samples, "target34.py", "keep_cpu_busy", 32)

    assert meta["mode"] == "wall"

    a, _ = sum_metrics(samples)
    d = int(meta["duration"])

    assert 0 < 0.9 * d < a < 2.1 * d


# Support for multiprocess is attach-like and seems to suffer from the same
# issues as attach tests on Windows.
@pytest.mark.xfail(platform.system() == "Windows", reason="Does not pass in Windows CI")
@allpythons()
def test_fork_multiprocess(py):
    result = austin("-Ci", "1ms", *python(py), target("target_mp.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    ps = processes(result.samples)
    assert len(ps) >= 3, ps

    meta = result.metadata
    assert meta["multiprocess"] == "on", meta
    assert meta["mode"] == "wall", meta

    assert has_frame(result.samples, "target_mp.py", "do")
    assert has_frame(result.samples, "target_mp.py", "fact", 31)


@allpythons()
def test_fork_full_metrics(py):
    result = austin("-i", "10ms", "-f", *python(py), target("target34.py"))
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.samples)) == 1
    ts = threads(result.samples)
    assert len(ts) == 2, ts

    assert has_frame(result.samples, "target34.py", "keep_cpu_busy", 32)
    assert b"Unwanted" not in result.stdout

    meta = result.metadata

    assert meta["mode"] == "full"

    wall, cpu, alloc, dealloc = sum_full_metrics(result.samples)
    d = int(meta["duration"])

    assert 0 < 0.9 * d < wall < 2.1 * d
    assert 0 < cpu <= wall
    assert alloc * dealloc


@pytest.mark.parametrize("exposure", (1, 2))
@pytest.mark.parametrize("children", ([], ["-C"]))
@allpythons()
def test_fork_exposure(py, exposure, children):
    result = austin(
        "-i",
        "100ms",
        *children,
        "-x",
        str(exposure),
        *python(py),
        target("sleepy.py"),
        "1",
        expect_fail=True if sys.platform == "win32" else 256 - signal.SIGINT,
    )

    assert has_frame(result.samples, "sleepy.py", "<module>")

    meta = result.metadata

    assert meta["mode"] == "wall"

    d = int(meta["duration"]) / 1e6  # seconds
    assert abs(d - exposure) <= 0.5


@variants
@allpythons(min=(3, 11))
def test_qualnames(py, austin):
    result = austin("-i", "1ms", *python(py), target("qualnames.py"))
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.samples)) == 1
    ts = threads(result.samples)
    assert len(ts) == 1

    assert has_frame(result.samples, "qualnames.py", "Foo.run")
    assert has_frame(result.samples, "qualnames.py", "Bar.run")


@allpythons()
def test_no_logging(py, monkeypatch):
    monkeypatch.setenv("AUSTIN_NO_LOGGING", "1")
    result = austin("-i", "1ms", *python(py), target("target34.py"))
    assert has_frame(result.samples, "target34.py", "keep_cpu_busy", 32)
    assert result.returncode == 0, result.stderr or result.stdout


@allpythons()
def test_max_page_size(py, monkeypatch):
    monkeypatch.setenv("AUSTIN_PAGE_SIZE_CAP", "1024")
    result = austin("-i", "1ms", *python(py), target("target34.py"))
    assert has_frame(result.samples, "target34.py", "keep_cpu_busy", 32)
    assert result.returncode == 0, result.stderr or result.stdout


@pytest.mark.skipif(sys.platform == "win32", reason="Terminate signal not supported")
@allpythons()
def test_fork_term_signal(py):
    austin.args = ("-i", "10ms", *python(py), target("sleepy.py"), "5")
    austin.expect_fail = True if sys.platform == "win32" else 256 - signal.SIGTERM

    duration = 1
    with austin as result:
        sleep(duration)
        result.terminate()

    meta = result.metadata

    assert int(meta["duration"])


@pytest.mark.skipif(sys.platform == "win32", reason="Interrupt signal not supported")
@allpythons()
def test_fork_int_signal(py):
    austin.args = ("-i", "10ms", *python(py), target("sleepy.py"), "5")
    austin.expect_fail = True if sys.platform == "win32" else 256 - signal.SIGINT

    duration = 1
    with austin as result:
        sleep(duration)
        os.kill(result.pid, signal.SIGINT)

    meta = result.metadata

    assert int(meta["duration"])


@pytest.mark.skipif(sys.platform == "win32", reason="UNIX only")
@pytest.mark.parametrize("children", ([], ["-C"]))
@allpythons()
@variants
def test_fork_exec(austin, py, children):
    result = austin("-i", "2ms", *children, *python(py), target("target_exec.py"))
    assert py in (result.stderr or result.stdout), result.stderr or result.stdout

    assert len(processes(result.samples)) == 1
    ts = threads(result.samples)
    assert len(ts) == 2

    assert has_frame(
        result.samples, filename="target34.py", function="keep_cpu_busy", line=32
    )
    assert b"Unwanted" not in result.stdout

    meta = result.metadata

    assert meta["mode"] == "wall"

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])

    assert 0 < a < 2.1 * d

    if austin == "austinp":
        ms = maps(result.stdout)
        assert len(ms) >= 2, ms
        assert [_ for _ in ms if "python" in _], ms
