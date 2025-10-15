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
import time
from collections import Counter
from shutil import rmtree
from test.utils import allpythons
from test.utils import austin
from test.utils import austinp
from test.utils import has_frame
from test.utils import requires_sudo
from test.utils import run_python
from test.utils import sum_metrics
from test.utils import target
from test.utils import threads
from test.utils import variants
from time import sleep

import pytest
from flaky import flaky


@requires_sudo
@pytest.mark.parametrize(
    "mode,mode_meta", [("-i", "wall"), ("-ci", "cpu"), ("-Ci", "wall"), ("-Cci", "cpu")]
)
@allpythons()
@variants
def test_attach_wall_time(austin, py, mode, mode_meta):
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)

        result = austin(mode, "2ms", "-p", str(p.pid))
        assert result.returncode == 0

        ts = threads(result.samples)
        assert len(ts) == 1

        assert has_frame(result.samples, filename="sleepy.py", function="<module>")

        meta = result.metadata

        assert meta["mode"] == mode_meta

        a, _ = sum_metrics(result.samples)
        d = int(meta["duration"])

        assert a <= 1.1 * d


@requires_sudo
@pytest.mark.parametrize("exposure", [1, 2])
@allpythons()
def test_attach_exposure(py, exposure):
    with run_python(py, target("sleepy.py"), "3") as p:
        result = austin(
            "-i",
            "100ms",
            "-x",
            str(exposure),
            "-p",
            str(p.pid),
            expect_fail=True if sys.platform == "win32" else 256 - signal.SIGINT,
        )

        assert has_frame(result.samples, filename="sleepy.py", function="<module>")

        meta = result.metadata

        d = int(meta["duration"]) / 1e6  # seconds

        assert abs(d - exposure) <= 0.5

        p.kill()


@requires_sudo
@allpythons()
def test_where(py):
    with run_python(py, target("sleepy.py"), sleep_after=1) as p:
        result = austin("-w", str(p.pid))
        assert result.returncode == 0

        assert "Process" in result.stdout
        assert "Thread" in result.stdout
        assert "sleepy.py" in result.stdout
        assert "<module>" in result.stdout


@requires_sudo
@allpythons()
def test_where_multiple_times(py):
    n = 10
    with run_python(py, target("sleepy.py"), str(n), sleep_after=1) as p:
        for _ in range(n):
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
        end = time.time() + 60
        while p.returncode is None and time.time() < end:
            sleep(0.2)
            result = austin("-Cw", str(p.pid))
            assert result.returncode == 0

            lines = Counter(result.stdout.splitlines())

            if sum(c for line, c in lines.items() if "Process" in line) >= 3:
                break
        else:
            raise AssertionError("expected number of processes")

        assert sum(c for line, c in lines.items() if "fact" in line) == 2
        (join_line,) = (line for line in lines if "join" in line)
        assert lines[join_line] == 1


@pytest.mark.xfail(reason="Fails in CI with some Python versions")
@requires_sudo
@allpythons()
def test_where_kernel(py):
    with run_python(py, target("sleepy.py"), sleep_after=1) as p:
        result = austinp("-kw", str(p.pid))
        assert result.returncode == 0

        assert "Process" in result.stdout
        assert "Thread" in result.stdout
        assert "sleepy.py" in result.stdout
        assert "<module>" in result.stdout
        assert "libc" in result.stdout
        assert "do_syscall" in result.stdout


@pytest.mark.parametrize("prefix", [[], ["unshare", "-p", "-f", "-r"]])
@pytest.mark.skipif(platform.system() != "Linux", reason="Linux only")
@pytest.mark.xfail(os.getenv("CI") == "true", reason="Fails in CI")
@requires_sudo
@allpythons()
def test_attach_container_like(py, tmp_path, prefix):
    """Test in container-like conditions.

    We test that we can still attach Austin to a running process even if we
    don't have access to the binary files to determine the location of the
    symbols. We also test against an interpreter started in a different PID
    namespace to emulate a container as closely as possible.
    """
    venv_path = tmp_path / ".venv"
    p = run_python(py, "-m", "venv", "--copies", str(venv_path))
    p.wait(30)
    assert 0 == p.returncode, "Virtual environment was created successfully"

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(venv_path / "lib")
    env["PATH"] = str(venv_path / "bin") + os.pathsep + env["PATH"]
    with run_python(
        py, target("sleepy.py"), "3", env=env, prefix=prefix, sleep_after=0.5
    ) as p:
        rmtree(venv_path)
        sleep(0.5)

        result = austin("-Cp", str(p.pid))
        assert result.returncode == 0

        ts = threads(result.stdout)
        assert len(ts) == 1

        assert has_frame(result.samples, filename="sleepy.py", function="<module>")

        meta = result.metadata

        a = sum_metrics(result.stdout)
        d = int(meta["duration"])

        assert abs(a - d) <= (a + d) * 0.25
