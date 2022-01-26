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

from subprocess import run
from test.utils import allpythons, austin, python, requires_sudo, run_python, target

import pytest


def valgrind(python_args: list[str], mode: str):
    try:
        return run(
            [
                "valgrind",
                "--error-exitcode=42",
                "--leak-check=full",
                "--show-leak-kinds=all",
                "--errors-for-leak-kinds=all",
                "--track-fds=yes",
                str(austin.path),
                f"-{mode}i",
                "1ms",
                "-t",
                "1s",
                "-o",
                "/dev/null",
                *python_args,
            ],
            capture_output=True,
            timeout=30,
            text=True,
        )
    except FileNotFoundError:
        pytest.skip("Valgrind not available")


@pytest.mark.parametrize("mode", ["", "s", "C", "Cs"])
@allpythons()
def test_valgrind_fork(py, mode):
    result = valgrind([*python(py), target()], mode)
    assert result.returncode == 0, "\n".join((result.stdout, result.stderr))


@requires_sudo
@pytest.mark.parametrize("mode", ["", "s", "C", "Cs"])
@allpythons()
def test_valgrind_attach(py, mode):
    with run_python(py, target("sleepy.py")) as p:
        result = valgrind(["-p", str(p.pid)], mode)
        assert result.returncode == 0, "\n".join((result.stdout, result.stderr))
        p.kill()
