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
import sys
from asyncio.subprocess import STDOUT
from collections import Counter
from collections import defaultdict
from functools import cached_property
from io import BytesIO
from itertools import count
from pathlib import Path
from shutil import rmtree
from subprocess import PIPE
from subprocess import CalledProcessError
from subprocess import CompletedProcess
from subprocess import Popen
from subprocess import TimeoutExpired
from subprocess import check_output
from tempfile import gettempdir
from test import PYTHON_VERSIONS
from time import sleep
from types import FrameType
from types import ModuleType
from typing import Dict
from typing import Iterator
from typing import List
from typing import Optional
from typing import Tuple
from typing import TypeVar
from typing import Union


if sys.platform == "win32":
    from subprocess import CREATE_NEW_PROCESS_GROUP


try:
    import pytest
except ImportError:
    pytest = None

from austin.events import AustinSample
from austin.format.mojo import MojoStreamReader


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
        case "Darwin" | "Linux":
            py = [f"python{version}"]
        case _:
            raise RuntimeError(f"Unsupported platform: {platform.system()}")

    try:
        check_output([*py, "-V"], stderr=STDOUT)
        return py
    except FileNotFoundError:
        return pytest.skip(f"Python {version} not found")


def gdb(cmds: list[str], *args: str) -> str:
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


def bt(binary: Path, pid: int) -> str:
    core_file = f"core.{pid}"
    if Path(core_file).is_file():
        return gdb(["bt full", "q"], str(binary), core_file)

    # On Ubuntu, apport puts crashes in /var/crash
    crash_dir = Path("/var/crash")
    if crash_dir.is_dir():
        crashes = list(crash_dir.glob("*.crash"))
        if crashes:
            # Take the last one
            crash = crashes[-1]
            target_dir = Path(crash.stem)
            apport_unpack(crash, target_dir)

            result = gdb(["bt full", "q"], str(binary), str(target_dir / "CoreDump"))

            crash.unlink()
            rmtree(str(target_dir))

            return result

    return "No core dump available."


def collect_logs(variant: str, pid: int) -> List[str]:
    needles: tuple[str, ...]
    match platform.system():
        case "Linux":
            with Path("/var/log/syslog").open(errors="replace") as logfile:
                needles = (f"{variant}[{pid}]", f"systemd-coredump[{pid}]")
                return [
                    f" logs for {variant}[{pid}] ".center(80, "="),
                    *(
                        line.strip().replace("#012", "\n")
                        for line in logfile.readlines()
                        if any(needle in line for needle in needles)
                    ),
                    f" end of logs for {variant}[{pid}] ".center(80, "="),
                ]

        case "Windows":
            with (Path(gettempdir()) / "austin.log").open() as logfile:
                needles = (f"{variant}[{pid}]",)
                return [
                    f" logs for {variant}[{pid}] ".center(80, "="),
                    *(
                        line.strip()
                        for line in logfile.readlines()
                        if any(needle in line for needle in needles)
                    ),
                    f" end of logs for {variant}[{pid}] ".center(80, "="),
                ]

        case _:
            return []


EXEEXT = ".exe" if platform.system() == "Windows" else ""


# Taken from the subprocess module. We make our own version that can also
# propagate the PID.
def run(
    *popenargs, input=None, capture_output=False, timeout=None, check=False, **kwargs
):
    if input is not None:
        if kwargs.get("stdin") is not None:
            raise ValueError("stdin and input arguments may not both be used.")
        kwargs["stdin"] = PIPE

    if capture_output:
        if kwargs.get("stdout") is not None or kwargs.get("stderr") is not None:
            raise ValueError(
                "stdout and stderr arguments may not be used with capture_output."
            )
        kwargs["stdout"] = PIPE
        kwargs["stderr"] = PIPE

    with Popen(*popenargs, **kwargs) as process:
        try:
            stdout, stderr = process.communicate(input, timeout=timeout)
        except TimeoutExpired as exc:
            exc.pid = process.pid
            process.kill()
            if platform.system() == "Windows":
                exc.stdout, exc.stderr = process.communicate()
            else:
                process.wait()
            raise
        except Exception as e:
            e.pid = process.pid
            process.kill()
            raise
        retcode = process.poll()
        if check and retcode:
            error = CalledProcessError(
                retcode, process.args, output=stdout, stderr=stderr
            )
            error.pid = process.pid
            raise error
    result = CompletedProcess(process.args, retcode, stdout, stderr)
    result.pid = process.pid

    return result


def print_logs(logs: List[str]) -> None:
    if logs:
        for log in logs:
            print(log)
    else:
        print("<< no logs available >>")


(DUMP_PATH := HERE.parent / "dumps").mkdir(exist_ok=True)


def dump_mojo(data: bytes) -> None:
    frame: Optional[FrameType] = sys._getframe(1)

    while not (frame is None or frame.f_code.co_name.startswith("test_")):
        frame = frame.f_back

    if frame is None:
        return

    test_name = frame.f_code.co_name

    for i in count(1):
        dump_file = DUMP_PATH / f"{test_name}_{i}.mojo"
        if not dump_file.is_file():
            dump_file.write_bytes(data)
            print(f"Dumped mojo data to {dump_file}")
            return


class Variant:
    ALL: list["Variant"] = []

    def __init__(self, name: str) -> None:
        super().__init__()

        self.name = name
        self.args = tuple()
        self.timeout = 60
        self.expect_fail = False
        self.convert = True

        path = (Path("src") / name).with_suffix(EXEEXT)
        if not path.is_file():
            path = Path(name).with_suffix(EXEEXT)

        self.path = path

        if self.path.is_file():
            self.ALL.append(self)

    @cached_property
    def help(self) -> str:
        try:
            return run(
                [str(self.path), "--help"],
                capture_output=True,
            ).stdout.decode()
        except (FileNotFoundError, RuntimeError):
            return "No help available for this variant."

    def __call__(
        self,
        *args: str,
        timeout: int = 60,
        convert: bool = True,
        expect_fail: Union[bool, int] = False,
    ) -> CompletedProcess:
        if not self.path.is_file():
            if "PYTEST_CURRENT_TEST" in os.environ:
                pytest.skip(f"{self} not available")
            else:
                raise FileNotFoundError(f"Binary {self.path} not found for {self}")

        extra_args = ["-b"] if "-b, --binary" in self.help else []

        try:
            flags = 0
            if sys.platform == "win32":
                flags = CREATE_NEW_PROCESS_GROUP

            result = run(
                [str(self.path)] + extra_args + list(args),
                capture_output=True,
                timeout=timeout,
                creationflags=flags,
            )
        except Exception as exc:
            if (pid := getattr(exc, "pid", None)) is not None:
                print_logs(collect_logs(self.name, pid))
            raise

        return self.prepare_result(result, expect_fail, convert)

    def prepare_result(self, result, expect_fail, convert):
        if result.returncode in (-11, 139):  # SIGSEGV
            print(bt(self.path, result.pid))

        # If we are writing to stdout, check if we need to convert the stream
        if result.stdout.startswith(b"MOJ"):
            if convert:
                try:
                    result.samples, result.metadata = parse_mojo(result.stdout)
                except Exception as e:
                    dump_mojo(result.stdout)
                    raise e
        else:
            result.stdout = result.stdout.decode()
        result.stderr = result.stderr.decode()

        logs = collect_logs(self.name, result.pid)
        result.logs = logs

        if isinstance(expect_fail, bool) and expect_fail is bool(result.returncode):
            return result

        if isinstance(expect_fail, int) and result.returncode == expect_fail:
            return result

        print_logs(logs)
        raise RuntimeError(
            f"Command {self.name} returned {result.returncode} "
            f"while expecting {expect_fail}. Output:\n{result.stdout}\n"
            f"Error:\n{result.stderr}"
        )

    def __enter__(self):
        if not self.path.is_file():
            if "PYTEST_CURRENT_TEST" in os.environ:
                pytest.skip(f"{self} not available")
            else:
                raise FileNotFoundError(f"Binary {self.path} not found for {self}")

        extra_args = ["-b"] if "-b, --binary" in self.help else []

        try:
            flags = 0
            if sys.platform == "win32":
                flags = CREATE_NEW_PROCESS_GROUP

            self.subprocess = Popen(
                [str(self.path)] + extra_args + list(self.args),
                stdout=PIPE,
                stderr=PIPE,
                creationflags=flags,
            )
        except Exception as exc:
            if (pid := getattr(exc, "pid", None)) is not None:
                print_logs(collect_logs(self.name, pid))
            raise

        return self.subprocess

    def __exit__(self, exc_type, exc_value, traceback):
        self.subprocess.wait(timeout=10)

        result = self.subprocess

        result.stdout = result.stdout.read()
        result.stderr = result.stderr.read()

        self.prepare_result(result, self.expect_fail, self.convert)

        self.args = tuple()
        self.timeout = 60
        self.expect_fail = False
        self.convert = True

        return False

    def __repr__(self) -> str:
        return f"Variant({self.name!r})"


austin = Variant("austin")
austinp = Variant("austinp")


def run_async(command: list[str], *args: str, env: dict | None = None) -> Popen:
    return Popen(command + list(args), stdout=PIPE, stderr=PIPE, env=env)


def run_python(
    version,
    *args: str,
    env: dict | None = None,
    prefix: list[str] = [],
    sleep_after: float | None = None,
) -> Popen:
    result = run_async(prefix + python(version), *args, env=env)

    if sleep_after is not None:
        sleep(sleep_after)

    return result


T = TypeVar("T")


def denoise(data: Iterator[T], threshold: float = 0.1) -> set[T]:
    c = Counter(data)
    try:
        m = max(c.values())
    except ValueError:
        return set()
    return {t for t, c in c.items() if c / m > threshold}


def processes(samples: List[AustinSample]) -> set[str]:
    return denoise(_.pid for _ in samples)


def threads(
    samples: List[AustinSample], threshold: float = 0.1
) -> set[tuple[str, str, str]]:
    return denoise(((_.pid, _.thread, _.iid) for _ in samples), threshold)


def maps(data: str) -> defaultdict[str, list[str]]:
    maps = defaultdict(list)
    for r, f in (
        _[7:].split(" ", maxsplit=1)
        for _ in data.splitlines()
        if _.startswith("# map:")
    ):
        maps[f].append(r)
    return maps


def has_frame(
    samples: List[AustinSample],
    filename,
    function,
    line=None,
    line_end=None,
    column=None,
    column_end=None,
):
    for sample in samples:
        if not sample.frames:
            continue
        for frame in sample.frames:
            if all(
                (
                    Path(frame.filename).name == filename,
                    frame.function == function,
                    line is None or frame.line == line,
                    line_end is None or frame.line_end == line_end,
                    column is None or frame.column == column,
                    column_end is None or frame.column_end == column,
                )
            ):
                return True
    return False


def sum_metrics(samples: List[AustinSample]) -> tuple[int, int]:
    return sum(_.metrics.time or 0 for _ in samples), sum(
        _.metrics.memory or 0 for _ in samples
    )


def sum_full_metrics(samples: List[AustinSample]) -> tuple[int, int, int, int]:
    mem = [_.metrics.memory or 0 for _ in samples]
    return (
        sum(_.metrics.time or 0 for _ in samples),
        sum(_.metrics.time or 0 for _ in samples if not _.idle),
        sum(_ for _ in mem if _ > 0),
        sum(_ for _ in mem if _ < 0),
    )


def parse_mojo(data: bytes) -> Tuple[List[AustinSample], Dict[str, str]]:
    mojo = MojoStreamReader(BytesIO(data))
    samples = [_ for _ in mojo if isinstance(_, AustinSample)]
    return samples, mojo.metadata


# Load from the utils scripts
def load_util(name: str) -> ModuleType:
    module_path = (Path(__file__).parent.parent / "utils" / name).with_suffix(".py")
    spec = importlib.util.spec_from_file_location(name, str(module_path))

    assert spec is not None and spec.loader is not None, spec

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    return module


if pytest is not None:
    variants = pytest.mark.parametrize("austin", Variant.ALL, ids=lambda v: v.name)

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
