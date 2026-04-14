import os
from pathlib import Path

import pytest

_uid = getattr(os, "getuid", lambda: 0)()

_MOJO_DIR = (
    Path(os.environ["AUSTIN_MOJO_DIR"])
    if "AUSTIN_MOJO_DIR" in os.environ
    else Path("test-profiles") / str(_uid)
)


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
    failed = getattr(getattr(request.node, "rep_call", None), "failed", True)
    if not failed:
        for p in saved_paths:
            p.unlink(missing_ok=True)
