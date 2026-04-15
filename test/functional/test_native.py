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
import shutil
import subprocess
from functools import lru_cache
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
    platform.system() not in ("Darwin", "Linux"),
    reason="Native stack sampling with -n is only supported on Darwin and Linux",
)

_IS_LINUX = platform.system() == "Linux"


def _python_linked_runtime(exe: str) -> list:
    """Return paths of Python runtime shared libraries linked by exe."""
    system = platform.system()
    try:
        if system == "Linux":
            out = subprocess.check_output(
                ["ldd", exe], stderr=subprocess.DEVNULL, text=True
            )
            paths = []
            for line in out.splitlines():
                # ldd lines: "  libpython3.x.so => /path/to/lib (0x...)"
                if "python" not in line.lower():
                    continue
                parts = line.split("=>")
                if len(parts) == 2:
                    path = parts[1].split()[0].strip()
                    if path and path != "(not":
                        paths.append(path)
            return paths
        elif system == "Darwin":
            out = subprocess.check_output(
                ["otool", "-L", exe], stderr=subprocess.DEVNULL, text=True
            )
            paths = []
            for line in out.splitlines()[1:]:
                path = line.strip().split()[0]
                if "python" in path.lower() or "Python" in path:
                    paths.append(path)
            return paths
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return []


@lru_cache(maxsize=None)
def _python_has_Py_RunMain_symbol(py: str) -> bool:
    """Return True if Py_RunMain is present in the Python runtime.

    On Linux, Austin's NATIVE mode uses DWARF CFI (.eh_frame) unwinding on
    x86-64 and frame-pointer walking on aarch64 (where the AAPCS mandates
    frame pointers).  CFI can unwind through binaries compiled without frame
    pointers, so we only need to verify the symbol exists — not that the
    binary was built with -fno-omit-frame-pointer.

    On macOS, the ABI mandates frame pointers on both x86-64 and arm64, so
    frame-pointer walking always reaches Py_RunMain if the symbol exists.

    We check both .symtab (nm) and .dynsym (nm -D) to handle stripped
    binaries where only the dynamic symbol table remains.
    """
    exe = shutil.which(f"python{py}")
    if exe is None:
        return False

    def has_symbol(path: str) -> bool:
        for cmd in [["nm", path], ["nm", "-D", path]]:
            try:
                out = subprocess.check_output(cmd, stderr=subprocess.DEVNULL, text=True)
                if "Py_RunMain" in out:
                    return True
            except (subprocess.CalledProcessError, FileNotFoundError):
                pass
        return False

    for path in [exe] + _python_linked_runtime(exe):
        if has_symbol(path):
            return True
    return False


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
                and filename_contains.lower() not in frame.filename.lower()
            ):
                continue
            return True
    return False


@allpythons()
def test_native_wall_time(py, save_mojo):
    result = austin("-n", "-i", "1ms", *python(py), target("target34.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(
        result.samples, filename="target34.py", function="keep_cpu_busy", line=32
    ), "Expected Python frame from target34.py"

    assert has_native_frame(
        result.samples, filename_contains="python"
    ), "Expected native frame from the Python runtime"

    if _python_has_Py_RunMain_symbol(py):
        assert has_native_frame(
            result.samples, function="Py_RunMain"
        ), "Expected Py_RunMain native frame from the Python runtime"

    meta = result.metadata
    assert meta["mode"] == "wall"

    a, _ = sum_metrics(result.samples)
    d = int(meta["duration"])
    assert 0 < a < 2.1 * d


@allpythons()
def test_native_interleaved(py):
    """At least one sample must contain both Python and native frames."""
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
def test_native_attach(py, save_mojo):
    """Native mode works when attaching to an already-running process."""
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)
        result = austin("-n", "-i", "2ms", "-p", str(p.pid))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(
        result.samples, filename="sleepy.py", function="<module>"
    ), "Expected Python frame from sleepy.py in attach mode"

    assert has_native_frame(
        result.samples, filename_contains="python"
    ), "Expected native frame from the Python runtime in attach mode"

    if _python_has_Py_RunMain_symbol(py):
        assert has_native_frame(
            result.samples, function="Py_RunMain"
        ), "Expected Py_RunMain native frame from the Python runtime in attach mode"

    meta = result.metadata
    assert meta["mode"] == "wall"


@requires_sudo
@allpythons()
def test_native_where(py):
    """--where output must include Python source information in native mode."""
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)
        result = austin("-n", "-w", str(p.pid))
    assert result.returncode == 0, result.stderr or result.stdout

    assert "sleepy.py" in result.stdout, result.stdout
    assert "<module>" in result.stdout, result.stdout

    if _python_has_Py_RunMain_symbol(py):
        assert (
            "Py_RunMain" in result.stdout
        ), "Expected Py_RunMain native frame in where output"


@allpythons()
@pytest.mark.skipif(not _IS_LINUX, reason="Linux-specific regression test")
def test_native_does_not_affect_cpu_time_linux(py):
    """Running without -n must still correctly filter idle threads in CPU mode.

    Previously, NATIVE being defined for plain austin caused py_thread__is_idle
    to always return False (bitmap never populated without seizing), making CPU
    mode degenerate to wall time and inflating metrics.
    """
    wall = austin("-i", "1ms", *python(py), target("target34.py"))
    cpu = austin("-ci", "1ms", *python(py), target("target34.py"))

    assert wall.returncode == 0, wall.stderr or wall.stdout
    assert cpu.returncode == 0, cpu.stderr or cpu.stdout

    wall_total, _ = sum_metrics(wall.samples)
    cpu_total, _ = sum_metrics(cpu.samples)

    # CPU time must be strictly less than wall time for a target that does
    # real sleep: the idle periods should be filtered out.
    assert cpu_total < wall_total, (
        f"CPU total ({cpu_total}) should be less than wall total ({wall_total}): "
        "idle threads may not be filtered correctly"
    )
