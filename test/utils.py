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
from asyncio.subprocess import STDOUT
from collections import Counter, defaultdict
from pathlib import Path
from subprocess import PIPE, CompletedProcess, Popen, check_output, run
from test import PYTHON_VERSIONS
from typing import Iterator, TypeVar

import pytest

HERE = Path(__file__).parent


def target(name: str = "target34.py") -> str:
    return str(HERE / "targets" / name)


def allpythons(min=None, max=None):
    def _(f):
        versions = PYTHON_VERSIONS
        if min is not None:
            versions = [_ for _ in versions if _ >= min]
        if max is not None:
            versions = [_ for _ in versions if _ <= max]
        return pytest.mark.parametrize(
            "py", [".".join([str(_) for _ in v]) for v in versions]
        )(f)

    return _


if platform.system() == "Darwin":
    BREW_PREFIX = check_output(["brew", "--prefix"], text=True, stderr=STDOUT).strip()


def python(version: str) -> list[str]:
    match platform.system():
        case "Windows":
            py = ["py", f"-{version}"]
        case "Darwin":
            py = [f"{BREW_PREFIX}/opt/python@{version}/bin/python3"]
        case "Linux":
            py = [f"python{version}"]
        case _:
            raise RuntimeError(f"Unsupported platform: {platform.system()}")

    try:
        check_output([*py, "-V"], stderr=STDOUT)
        return py
    except FileNotFoundError:
        pytest.skip(f"Python {version} not found")


def gdb(cmds: list[str], *args: tuple[str]) -> str:
    return check_output(
        ["gdb"] + [_ for cs in (("-ex", _) for _ in cmds) for _ in cs] + list(args),
        stderr=STDOUT,
    ).decode()


def bt(binary: Path) -> str:
    if Path("core").is_file():
        return gdb(["bt full", "q"], str(binary), "core")
    return "No core dump available."


EXEEXT = ".exe" if platform.system() == "Windows" else ""

class Variant(str):

    ALL: list["Variant"] = []

    def __init__(self, name: str) -> None:
        super().__init__()

        austin_exe = f"{name}{EXEEXT}"
        path = Path("src") / austin_exe
        if not path.is_file():
            path = Path(austin_exe)

        self.path = path

        self.ALL.append(self)

    def __call__(self, *args: tuple[str], timeout: int = 60) -> CompletedProcess:
        if not self.path.is_file():
            pytest.skip(f"Variant '{self}' not available")

        result = run(
            [str(self.path)] + list(args),
            capture_output=True,
            timeout=timeout,
            text=True,
            errors="ignore",
        )

        if result.returncode in (-11, 139):  # SIGSEGV
            print(bt(self.path))

        return result


austin = Variant("austin")
austinp = Variant("austinp")

variants = pytest.mark.parametrize("austin", Variant.ALL)


def run_async(command: list[str], *args: tuple[str]) -> Popen:
    return Popen(command + list(args), stdout=PIPE, stderr=PIPE)


def run_python(version, *args: tuple[str]) -> Popen:
    return run_async(python(version), *args)


def samples(data: str) -> Iterator[bytes]:
    return (_ for _ in data.splitlines() if _ and _[0] == "P")


T = TypeVar("T")

def denoise(data: Iterator[T], threshold: float = 0.1) -> set[T]:
    c = Counter(data)
    try:
        m = max(c.values())
    except ValueError:
        return set()
    return {t for t, c in c.items() if c / m > threshold}


def processes(data: str) -> set[str]:
    return denoise(_.partition(";")[0] for _ in samples(data))


def threads(data: str, threshold: float = 0.1) -> set[tuple[str, str]]:
    return denoise(
        tuple(_.rpartition(" ")[0].split(";", maxsplit=2)[:2]) for _ in samples(data)
    )


def metadata(data: str) -> dict[str, str]:
    return dict(
        _[1:].strip().split(": ", maxsplit=1)
        for _ in data.splitlines()
        if _ and _[0] == "#"
    )


def maps(data: str) -> defaultdict[str, list[str]]:
    maps = defaultdict(list)
    for r, f in (_[7:].split(" ", maxsplit=1) for _ in data.splitlines() if _.startswith("# map:")):
        maps[f].append(r)
    return maps


def has_pattern(data: str, pattern: str) -> bool:
    for _ in samples(data):
        if pattern in _:
            return True
    return False


def sum_metric(data: str) -> int:
    return sum(int(_.rpartition(" ")[-1]) for _ in samples(data))


def sum_metrics(data: str) -> tuple[int, int, int, int]:
    wall = cpu = alloc = dealloc = 0
    for t, i, m in (
        _.rpartition(" ")[-1].split(",", maxsplit=2) for _ in samples(data)
    ):
        time = int(t)
        wall += time
        if i == "0":
            cpu += time

        memory = int(m)
        if memory > 0:
            alloc += memory
        else:
            dealloc += memory

    return wall, cpu, alloc, dealloc


def compress(data: str) -> str:
    output: list[str] = []
    stacks: dict[str, int] = {}
    for _ in (_.strip() for _ in data.splitlines()):
        if not _ or _[0] == "#":
            output.append(_)
            continue

        stack, _, metric = _.rpartition(" ")
        stacks[stack] = stacks.setdefault(stack, 0) + int(metric)

    return "\n".join(output) + "\n".join((f"{k} {v}" for k, v in stacks.items()))


match platform.system():
    case "Windows":
        requires_sudo = no_sudo = lambda f: f
    case _:
        requires_sudo = pytest.mark.skipif(
            os.geteuid() != 0, reason="Requires superuser privileges"
)
        no_sudo = pytest.mark.skipif(
            os.geteuid() == 0, reason="Must not have superuser privileges"
        )
