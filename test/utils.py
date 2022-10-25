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

import importlib
import os
import platform
from asyncio.subprocess import STDOUT
from collections import Counter, defaultdict
from io import BytesIO, StringIO
from pathlib import Path
from shutil import rmtree
from subprocess import PIPE, CompletedProcess, Popen, check_output, run
from test import PYTHON_VERSIONS
from time import sleep
from types import ModuleType
from typing import Iterator, TypeVar

import pytest
from austin.format.mojo import MojoFile

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
    match pl := platform.system():
        case "Windows":
            py = ["py", f"-{version}"]
        case "Darwin" | "Linux":
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
        ["gdb", "-q", "-batch"]
        + [_ for cs in (("-ex", _) for _ in cmds) for _ in cs]
        + list(args),
        stderr=STDOUT,
    ).decode()


def apport_unpack(report: Path, target_dir: Path):
    return check_output(
        ["apport-unpack", str(report), str(target_dir)],
        stderr=STDOUT,
    ).decode()


def bt(binary: Path) -> str:
    if Path("core").is_file():
        return gdb(["bt full", "q"], str(binary), "core")

    # On Ubuntu, apport puts crashes in /var/crash
    crash_dir = Path("/var/crash")
    if crash_dir.is_dir():
        crashes = list(crash_dir.glob("*.crash"))
        if crashes:
            # Take the last one
            crash = crashes[-1]
            target_dir = Path(crash.stem)
            apport_unpack(crash, target_dir)

            result = gdb(["bt full", "q"], str(binary), target_dir / "CoreDump")

            crash.unlink()
            rmtree(str(target_dir))

            return result

    return "No core dump available."


EXEEXT = ".exe" if platform.system() == "Windows" else ""


class Variant(str):

    ALL: list["Variant"] = []

    def __init__(self, name: str) -> None:
        super().__init__()

        path = (Path("src") / name).with_suffix(EXEEXT)
        if not path.is_file():
            path = Path(name).with_suffix(EXEEXT)

        self.path = path

        self.ALL.append(self)

    def __call__(
        self, *args: str, timeout: int = 60, mojo: bool = False
    ) -> CompletedProcess:
        if not self.path.is_file():
            pytest.skip(f"Variant '{self}' not available")

        mojo_args = ["-b"] if mojo else []

        result = run(
            [str(self.path)] + mojo_args + list(args),
            capture_output=True,
            timeout=timeout,
        )

        if result.returncode in (-11, 139):  # SIGSEGV
            print(bt(self.path))

        if mojo and not ({"-o", "-w", "--output", "--where"} & set(args)):
            # We produce MOJO binary data only if we are not writing to file
            # or using the "where" option.
            result.stdout = demojo(result.stdout)
        else:
            result.stdout = result.stdout.decode()
        result.stderr = result.stderr.decode()

        return result


austin = Variant("austin")
austinp = Variant("austinp")

variants = pytest.mark.parametrize("austin", Variant.ALL)


def run_async(command: list[str], *args: tuple[str]) -> Popen:
    return Popen(command + list(args), stdout=PIPE, stderr=PIPE)


def run_python(version, *args: tuple[str], sleep_after: float | None = None) -> Popen:
    result = run_async(python(version), *args)

    if sleep_after is not None:
        sleep(sleep_after)

    return result


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
    meta = dict(
        _[1:].strip().split(": ", maxsplit=1)
        for _ in data.splitlines()
        if _ and _[0] == "#" and not _.startswith("# map:")
    )

    for v in ("austin", "python"):
        if v in meta:
            meta[v] = tuple(int(_) for _ in meta[v].split("."))

    return meta


def maps(data: str) -> defaultdict[str, list[str]]:
    maps = defaultdict(list)
    for r, f in (
        _[7:].split(" ", maxsplit=1)
        for _ in data.splitlines()
        if _.startswith("# map:")
    ):
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
    stacks: dict[str, int] = {}

    for _ in (_.strip() for _ in data.splitlines() if _ and _[0] == "P"):
        stack, _, metric = _.rpartition(" ")
        stacks[stack] = stacks.setdefault(stack, 0) + int(metric)

    compressed_stacks = "\n".join((f"{k} {v}" for k, v in stacks.items()))

    output = (
        f"# Metadata\n{metadata(data)}\n\n# Stacks\n{compressed_stacks or '<no data>'}"
    )

    ms = maps(data)
    if ms:
        output = f"# Maps\n{list(ms.keys())}\n\n" + output

    return output


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


def demojo(data: bytes) -> str:
    result = StringIO()

    for e in MojoFile(BytesIO(data)).parse():
        result.write(e.to_austin())

    return result.getvalue()


mojo = pytest.mark.parametrize("mojo", [False, True])


# Load from the utils scripts
def load_util(name: str) -> ModuleType:
    module_path = (Path(__file__).parent.parent / "utils" / name).with_suffix(".py")
    spec = importlib.util.spec_from_file_location(name, str(module_path))

    assert spec is not None and spec.loader is not None, spec

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module
