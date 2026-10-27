import os
import subprocess
from pathlib import Path

import pytest

_uid = getattr(os, "getuid", lambda: 0)()

_MOJO_DIR = (
    Path(os.environ["AUSTIN_MOJO_DIR"])
    if "AUSTIN_MOJO_DIR" in os.environ
    else Path("test-profiles") / str(_uid)
)


_NATIVE_EXT_SRC = Path(__file__).parent.parent / "native"

_installed_for: set[str] = set()


def _python_exe(py: str) -> list[str] | None:
    """Return the argv prefix for the given Python version, or None if not found.

    Mirrors the platform-specific logic in test.utils.python() so that
    Windows (py launcher) and POSIX (pythonX.Y) are both handled correctly.
    """
    import platform as _platform
    from subprocess import check_output, STDOUT, CalledProcessError

    if _platform.system() == "Windows":
        cmd = ["py", f"-{py}"]
    else:
        cmd = [f"python{py}"]

    try:
        check_output([*cmd, "-V"], stderr=STDOUT)
        return cmd
    except (FileNotFoundError, CalledProcessError):
        return None


def install_native_ext(py: str) -> None:
    """Install test/native/ into the given Python version's site-packages.

    Uses pip install to build and install the native_ext C extension.
    Results are cached per Python version for the duration of the process.
    """
    if py in _installed_for:
        return
    cmd = _python_exe(py)
    if cmd is None:
        return
    subprocess.run(
        [*cmd, "-m", "pip", "install", "--quiet", str(_NATIVE_EXT_SRC)],
        check=True,
    )
    _installed_for.add(py)


@pytest.fixture
def native_ext(request):
    """Build and install native_ext for the Python version under test.

    Reads the ``py`` parametrize value from the current test node and calls
    ``install_native_ext`` so the C extension is available when the target
    script runs.  Results are cached per Python version for the process lifetime.
    """
    py = request.node.callspec.params["py"]
    install_native_ext(py)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


@pytest.fixture
def save_mojo(request):
    """Yield a callable that saves raw MOJO bytes to a file.

    If the test fails, the file is kept (and can be uploaded as a CI
    artifact).  If the test passes, the file is deleted to avoid clutter.

    Usage in tests::

        def test_something(py, save_mojo):
            result = austin("-n", ...)
            save_mojo(result.stdout, suffix="wall")
    """
    saved_paths = []

    def _save(data: bytes, suffix: str = "") -> Path:
        _MOJO_DIR.mkdir(parents=True, exist_ok=True)
        name = request.node.name.replace("/", "_").replace("[", "-").replace("]", "")
        if suffix:
            name = f"{name}-{suffix}"
        path = _MOJO_DIR / f"{name}-{os.getpid()}.mojo"
        path.write_bytes(data)
        saved_paths.append(path)
        return path

    yield _save

    # Clean up on pass; keep on failure so CI can upload the files.
    # In CI (GITHUB_ACTIONS), always keep profiles for artifact collection.
    if os.environ.get("CI"):
        return
    failed = getattr(getattr(request.node, "rep_call", None), "failed", True)
    if not failed:
        for p in saved_paths:
            p.unlink(missing_ok=True)
