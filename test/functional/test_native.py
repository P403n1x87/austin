# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2018-2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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
from test.utils import allpythons
from test.utils import austin
from test.utils import has_frame
from test.utils import python
from test.utils import requires_sudo
from test.utils import run_python
from test.utils import sum_metrics
from test.utils import target
from time import sleep

import pytest

pytestmark = pytest.mark.skipif(
    platform.system() != "Darwin",
    reason="Native stack sampling with -n is only supported on Darwin",
)


def has_native_frame(samples, function=None, filename_contains=None):
    """Check whether any sample contains a native (non-Python) frame."""
    for sample in samples:
        for frame in sample.frames:
            if frame.line != 0:
                # Python frames always have a line number; skip them.
                continue
            if function is not None and function not in frame.function:
                continue
            if (
                filename_contains is not None
                and filename_contains not in frame.filename
            ):
                continue
            return True
    return False


@requires_sudo
@allpythons()
def test_native_wall_time_darwin(py):
    result = austin("-n", "-i", "1ms", *python(py), target("target34.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(
        result.samples, filename="target34.py", function="keep_cpu_busy", line=32
    )

    assert has_native_frame(
        result.samples, function="Py_RunMain"
    ), "Expected Py_RunMain native frame from the Python runtime"

    meta = result.metadata
    assert meta["mode"] == "wall"

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])
    assert 0 < a < 2.1 * d


@requires_sudo
@allpythons()
def test_native_interleaved_darwin(py):
    """Python frames must be interleaved with native frames — verifies that
    at least one sample contains both a Python frame and a native frame from
    the Python runtime library."""
    result = austin("-n", "-i", "1ms", *python(py), target("target34.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    found_interleaved = False
    for sample in result.samples:
        has_py = any(frame.line != 0 for frame in sample.frames)
        has_nat = any(
            frame.line == 0 and "python" in frame.filename.lower()
            for frame in sample.frames
        )
        if has_py and has_nat:
            found_interleaved = True
            break

    assert (
        found_interleaved
    ), "Expected at least one sample with both Python and native frames interleaved"


@requires_sudo
@allpythons()
def test_native_attach_darwin(py):
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)
        result = austin("-n", "-i", "2ms", "-p", str(p.pid))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, filename="sleepy.py", function="<module>")
    assert has_native_frame(
        result.samples, function="Py_RunMain"
    ), "Expected Py_RunMain native frame from the Python runtime in attach mode"

    meta = result.metadata
    assert meta["mode"] == "wall"


@requires_sudo
@allpythons()
def test_native_where_darwin(py):
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)
        result = austin("-n", "-w", str(p.pid))
    assert result.returncode == 0, result.stderr or result.stdout

    assert "sleepy.py" in result.stdout, result.stdout
    assert "<module>" in result.stdout, result.stdout
    assert (
        "Py_RunMain" in result.stdout
    ), "Expected Py_RunMain native frame in where output"
