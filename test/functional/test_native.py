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
from test.utils import threads
from time import sleep

import pytest

_IS_LINUX = platform.system() == "Linux"
_IS_WIN = platform.system() == "Windows"


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

    On Linux, Austin's native mode uses DWARF CFI (.eh_frame) unwinding on
    x86-64 and frame-pointer walking on aarch64 (where the AAPCS mandates
    frame pointers).  CFI can unwind through binaries compiled without frame
    pointers, so we only need to verify the symbol exists — not that the
    binary was built with -fno-omit-frame-pointer.

    On macOS, the ABI mandates frame pointers on both x86-64 and arm64, so
    frame-pointer walking always reaches Py_RunMain if the symbol exists.

    On Windows, DbgHelp resolves symbols from PE exports and PDBs.  We check
    for the export via dumpbin when available, otherwise assume the symbol
    exists (CPython on Windows always exports it).
    """
    exe = shutil.which(f"python{py}")
    if exe is None:
        return False

    if _IS_WIN:
        # On Windows, Py_RunMain is always exported by the Python DLL.
        # Try dumpbin if available; otherwise assume True.
        try:
            out = subprocess.check_output(
                ["dumpbin", "/exports", exe],
                stderr=subprocess.DEVNULL,
                text=True,
            )
            return "Py_RunMain" in out
        except (subprocess.CalledProcessError, FileNotFoundError):
            return True

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

    assert has_native_frame(result.samples, filename_contains="python"), (
        "Expected native frame from the Python runtime"
    )

    if _python_has_Py_RunMain_symbol(py):
        assert has_native_frame(result.samples, function="Py_RunMain"), (
            "Expected Py_RunMain native frame from the Python runtime"
        )

    meta = result.metadata
    assert meta["mode"] == "wall"

    # Exclude non-Python threads (iid == -1) from the time sum: target34.py has
    # exactly 2 Python threads, so total attributed time should be < 2.1 * d.
    # Non-Python OS threads (Python runtime internals, etc.) are now also
    # sampled in native mode and would push the total above the expected range.
    python_samples = [s for s in result.samples if s.iid is not None and s.iid >= 0]
    a, _ = sum_metrics(python_samples)
    d = int(meta["duration"])
    assert 0 < a < 2.1 * d


@allpythons()
def test_native_interleaved(py, save_mojo):
    """At least one sample must contain both Python and native frames."""
    result = austin("-n", "-i", "1ms", *python(py), target("target34.py"))
    save_mojo(result.stdout)
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

    assert found_interleaved, (
        "Expected at least one sample with both Python and native frames interleaved"
    )


@requires_sudo
@allpythons()
def test_native_attach(py, save_mojo):
    """Native mode works when attaching to an already-running process."""
    with run_python(py, target("sleepy.py"), "2") as p:
        sleep(0.5)
        result = austin("-n", "-i", "2ms", "-p", str(p.pid))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, filename="sleepy.py", function="<module>"), (
        "Expected Python frame from sleepy.py in attach mode"
    )

    assert has_native_frame(result.samples, filename_contains="python"), (
        "Expected native frame from the Python runtime in attach mode"
    )

    if _python_has_Py_RunMain_symbol(py):
        assert has_native_frame(result.samples, function="Py_RunMain"), (
            "Expected Py_RunMain native frame from the Python runtime in attach mode"
        )

    meta = result.metadata
    assert meta["mode"] == "wall"


@requires_sudo
@allpythons()
def test_native_where(py):
    """--where output must include Python source information in native mode."""
    with run_python(py, target("sleepy.py"), "5") as p:
        sleep(1.5)
        result = austin("-n", "-w", str(p.pid))
    assert result.returncode == 0, result.stderr or result.stdout

    assert "sleepy.py" in result.stdout, result.stdout
    assert "<module>" in result.stdout, result.stdout

    if _python_has_Py_RunMain_symbol(py):
        assert "Py_RunMain" in result.stdout, (
            "Expected Py_RunMain native frame in where output"
        )


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
    # real sleep with the GIL: the idle periods should be filtered out.
    if not py.endswith("t"):
        assert cpu_total < wall_total, (
            f"CPU total ({cpu_total}) should be less than wall total ({wall_total}): "
            "idle threads may not be filtered correctly"
        )


# ---- Non-Python OS thread sampling -----------------------------------------


@requires_sudo
@allpythons()
def test_native_non_python_thread(py, native_ext):
    """Native mode must emit samples for OS threads with no PyThreadState.

    The target imports native_ext and starts two threads named "Native-0"
    and "Native-1" that have no PyThreadState.  Austin should include them in
    the output with their OS thread name and only native frames.
    """
    result = austin("-n", "-i", "1ms", *python(py), target("target_native_thread.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    # Use threshold=0 so Native is not filtered out by the denoise step
    # even if it appears in fewer samples than the Python main thread.
    thread_names = {name for _, name, _ in threads(result.samples, threshold=0)}
    assert {
        "Native-0",
        "Native-1",
    } <= thread_names, (
        f"Expected 'Native-0' and 'Native-1' threads in samples; got: {thread_names}"
    )

    # All frames for each native thread must be native (line == 0).
    for sample in result.samples:
        if not sample.thread.startswith("Native-"):
            continue
        assert all(frame.line == 0 for frame in sample.frames), (
            f"Expected only native frames for {sample.thread}, got: {sample.frames}"
        )


# ---- Windows unwind quality checks -----------------------------------------

# These function names appear in a known garbage chain caused by cold-block
# mislabeling on Windows x64 (Python 3.10).  Any sample where one of these
# functions appears as a *caller* of WaitForSingleObjectEx is bogus — list
# operations can never legitimately call WaitForSingleObjectEx.
#
# Note: PyObject_GC_UnTrack is excluded.  Without PDB symbols, take_gil
# (which genuinely calls WaitForSingleObjectEx to acquire the GIL) is
# mislabeled as PyObject_GC_UnTrack because it shares the same .pdata
# RUNTIME_FUNCTION as that export.  The call relationship is real even if
# the label is wrong, so flagging it would be a false positive.
_WIN_GARBAGE_CALLERS = {"PyList_Reverse", "PyList_Append"}


def _has_garbage_win_unwind(samples) -> list[str]:
    """Return descriptions of samples that show the known garbage unwind chain.

    Austin frames are ordered outermost-first: frames[0] is the thread root and
    frames[-1] is the leaf.  Callers of WaitForSingleObjectEx therefore appear
    at *lower* indices (closer to [0]) than WaitForSingleObjectEx itself.
    """
    bad = []
    for sample in samples:
        frames = sample.frames
        for i, frame in enumerate(frames):
            if "WaitForSingleObjectEx" not in frame.function:
                continue
            # Check up to 4 frames outward (lower indices = callers)
            lo = max(0, i - 4)
            for j in range(lo, i):
                if any(g in frames[j].function for g in _WIN_GARBAGE_CALLERS):
                    bad.append(
                        f"thread={sample.thread}: "
                        + " -> ".join(f.function for f in frames[j : i + 2])
                    )
                    break
    return bad


@allpythons()
@pytest.mark.skipif(not _IS_WIN, reason="Windows-specific unwind quality check")
def test_native_no_garbage_unwind_win(py, save_mojo):
    """On Windows, GIL-waiting threads must not show list/GC functions above WaitForSingleObjectEx.

    A false-positive epilog detection in the pdata unwinder can produce a garbage
    call chain like PyList_Reverse -> PyEval_RestoreThread -> PyObject_GC_UnTrack
    -> WaitForSingleObjectEx.  This test catches that regression.
    """
    result = austin("-n", "-i", "1ms", *python(py), target("target34.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    bad = _has_garbage_win_unwind(result.samples)
    assert not bad, (
        "Garbage unwind chain detected on Windows:\n"
        + "\n".join(f"  {b}" for b in bad[:10])
        + f"\n\naustin stderr:\n{result.stderr}"
    )
