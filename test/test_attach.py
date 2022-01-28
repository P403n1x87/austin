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
from collections import Counter
from test.utils import allpythons as _allpythons
from test.utils import (
    austin,
    austinp,
    compress,
    has_pattern,
    metadata,
    requires_sudo,
    run_python,
    sum_metric,
    target,
    threads,
)
from time import sleep

import pytest
from flaky import flaky


def allpythons():
    # Attach tests fail on Windows for Python < 3.7
    return _allpythons(min=(3, 7) if platform.system() == "Windows" else None)


@flaky(max_runs=3)
@requires_sudo
@pytest.mark.parametrize("heap", [tuple(), ("-h", "0"), ("-h", "64")])
@pytest.mark.parametrize(
    "mode,mode_meta", [("-i", "wall"), ("-si", "cpu"), ("-Ci", "wall"), ("-Csi", "cpu")]
)
@allpythons()
def test_attach_wall_time(py, mode, mode_meta, heap):
    with run_python(py, target("sleepy.py")) as p:
        sleep(0.5)

        result = austin(mode, f"10ms", *heap, "-p", str(p.pid))
        assert result.returncode == 0

        ts = threads(result.stdout)
        assert len(ts) == 1, compress(result.stdout)

        assert has_pattern(result.stdout, "sleepy.py:<module>:"), compress(
            result.stdout
        )

        meta = metadata(result.stdout)

        assert meta["mode"] == mode_meta

        a = sum_metric(result.stdout)
        d = int(meta["duration"])

        assert a <= d


@flaky
@requires_sudo
@pytest.mark.parametrize("exposure", [1, 2])
@allpythons()
def test_attach_exposure(py, exposure):
    with run_python(py, target("sleepy.py"), "3") as p:
        result = austin("-i", "1ms", "-x", str(exposure), "-p", str(p.pid))
        assert result.returncode == 0

        assert has_pattern(result.stdout, "sleepy.py:<module>:"), compress(
            result.stdout
        )

        meta = metadata(result.stdout)

        a = sum_metric(result.stdout)
        d = int(meta["duration"])

        assert exposure * 800000 <= d < exposure * 1200000

        p.kill()


@requires_sudo
@allpythons()
def test_where(py):
    with run_python(py, target("sleepy.py")) as p:
        sleep(1)
        result = austin("-w", str(p.pid))
        assert result.returncode == 0

        assert "Process" in result.stdout
        assert "Thread" in result.stdout
        assert "sleepy.py" in result.stdout
        assert "<module>" in result.stdout


@flaky
@requires_sudo
@pytest.mark.xfail(platform.system() == "Windows", reason="Does not pass in Windows CI")
@allpythons()
def test_where_multiprocess(py):
    with run_python(py, target("target_mp.py")) as p:
        while p.returncode is None:
            sleep(0.2)
            result = austin("-Cw", str(p.pid))
            assert result.returncode == 0

            lines = Counter(result.stdout.splitlines())

            if sum(c for line, c in lines.items() if "Process" in line) >= 3:
                break
        else:
            assert False, result.stdout

        assert sum(c for line, c in lines.items() if "fact" in line) == 2, result.stdout
        (join_line,) = (line for line in lines if "join" in line)
        assert lines[join_line] == 1, result.stdout


@flaky(max_runs=3)
@requires_sudo
@allpythons()
def test_where_kernel(py):
    with run_python(py, target("sleepy.py")) as p:
        sleep(1)
        result = austinp("-kw", str(p.pid))
        assert result.returncode == 0

        assert "Process" in result.stdout, result.stdout
        assert "Thread" in result.stdout, result.stdout
        assert "sleepy.py" in result.stdout, result.stdout
        assert "<module>" in result.stdout, result.stdout
        assert "__select" in result.stdout, result.stdout
        assert "libc" in result.stdout, result.stdout
        assert "do_syscall" in result.stdout, result.stdout
