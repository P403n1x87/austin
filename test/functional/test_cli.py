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
import sys
from pathlib import Path
from subprocess import run
from test.error import AustinError
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
    CC = os.getenv("CC", "gcc")

    if platform.system() == "Windows":
        exe_name = "notpython.exe"
    else:
        exe_name = "notpython"
    dest = Path(target("notpython.c")).parent / exe_name
    if not dest.exists():
        result = run([CC, target("notpython.c"), "-o", str(dest)])
        assert result.returncode == 0
        dest.chmod(0o755)

    result = austin(str(dest), expect_fail=AustinError.VERSION)

    assert "Cannot determine" in result.stderr


def test_cli_short_lived():
    result = austin(
        "src\\austin.exe" if platform.system() == "Windows" else "src/austin",
        sys.executable,
        "-c",
        "print('Hello World')",
        expect_fail=AustinError.OS,
    )

    assert "too quickly" in result.stderr


def test_cli_invalid_command():
    result = austin("snafubar", expect_fail=AustinError.OS)

    assert "Cannot launch" in (result.stderr or result.stdout)


def test_cli_invalid_pid():
    result = austin("-p", "9999999", expect_fail=AustinError.OS)

    assert "Cannot attach" in result.stderr


@pytest.mark.skipif(
    platform.system() == "Windows", reason="No permission issues on Windows"
)
@no_sudo
def test_cli_permissions():
    with run_python("5", target("sleepy.py")) as p:
        result = austin("-i", "1ms", "-p", str(p.pid), expect_fail=AustinError.PERM)
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
        expect_fail=AustinError.PERM,
    )

    assert "Insufficient permissions" in result.stderr, result.stderr
