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
import sys
from test.utils import austin
from test.utils import no_sudo
from test.utils import run_python
from test.utils import target

import pytest


def test_cli_no_arguments():
    result = austin()
    assert result.returncode == 0
    assert "Usage:" in result.stdout
    assert not result.stderr


def test_cli_no_python():
    match platform.system():
        case "Windows":
            cmd = ["cmd.exe", "/c", "sleep 2"]
        case _:
            cmd = ["bash", "-c", "sleep 2"]

    result = austin(*cmd, expect_fail=32)

    assert result.returncode == 32
    assert "not a Python" in result.stderr or "Cannot launch" in result.stderr


def test_cli_short_lived():
    result = austin(
        "src\\austin.exe" if platform.system() == "Windows" else "src/austin",
        sys.executable,
        "-c",
        "print('Hello World')",
        expect_fail=33,
    )
    assert result.returncode == 33
    assert "too quickly" in result.stderr


def test_cli_invalid_command():
    result = austin("snafubar", expect_fail=33)
    assert result.returncode == 33
    assert "Cannot launch" in (result.stderr or result.stdout)


def test_cli_invalid_pid():
    result = austin("-p", "9999999", expect_fail=36)

    assert result.returncode == 36
    assert "Cannot attach" in result.stderr


@pytest.mark.skipif(
    platform.system() == "Windows", reason="No permission issues on Windows"
)
@no_sudo
def test_cli_permissions():
    with run_python("3", target("sleepy.py")) as p:
        result = austin("-i", "1ms", "-p", str(p.pid), expect_fail=37)
        assert result.returncode == 37, result.stderr
        assert "Insufficient permissions" in result.stderr, result.stderr


@pytest.mark.skipif(
    platform.system() != "Darwin",
    reason="Only Darwin requires sudo in all cases",
)
@no_sudo
def test_cli_permissions_darwin():
    result = austin(
        "-i",
        "1ms",
        "python3.10",
        "-c",
        "from time import sleep; sleep(1)",
        expect_fail=37,
    )
    assert result.returncode == 37, result.stderr
    assert "Insufficient permissions" in result.stderr, result.stderr
