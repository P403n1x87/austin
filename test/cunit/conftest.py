import os
import sys
import typing as t
from pathlib import Path
from subprocess import PIPE
from subprocess import run
from test.cunit import SRC
from test.utils import bt
from types import FunctionType

import pytest


class SegmentationFault(Exception):
    pass


class CUnitTestFailure(Exception):
    pass


def pytest_configure(config: pytest.Config) -> None:
    # register an additional marker
    config.addinivalue_line("markers", "exitcode(code): the expected exit code")


def pytest_pycollect_makeitem(
    collector: pytest.Collector, name: str, obj: t.Any
) -> None:
    if (
        not os.getenv("PYTEST_CUNIT")
        and isinstance(obj, FunctionType)
        and name.startswith("test_")
    ):
        obj.__cunit__ = (str(collector.fspath), name)


def cunit(
    module: str, name: str, full_name: str, exit_code: int = 0
) -> t.Callable[[t.Any, t.Any], None]:
    def _(*_, **__):
        test = f"{module}::{name}"
        env = os.environ.copy()
        env["PYTEST_CUNIT"] = full_name

        result = run(
            [sys.executable, "-m", "pytest", "-svv", test],
            stdout=PIPE,
            stderr=PIPE,
            env=env,
        )

        if result.returncode == exit_code:
            return

        if result.returncode == -11:
            binary_name = Path(module).stem.replace("test_", "")
            raise SegmentationFault(bt((SRC / binary_name).with_suffix(".so")))

        raise CUnitTestFailure(
            f"\n{result.stdout.decode()}\n"
            f"Process terminated with exit code {result.returncode} "
            "(expected {exit_code})"
        )

    return _


def pytest_collection_modifyitems(
    session: pytest.Session,
    config: pytest.Config,
    items: list[pytest.Item],
) -> None:
    if test_name := os.getenv("PYTEST_CUNIT"):
        # We are inside the sandbox process. We select the only test we care
        items[:] = [_ for _ in items if _.name == test_name]
        return

    for item in items:
        if hasattr(item._obj, "__cunit__"):
            exit_code_marker = list(item.iter_markers(name="exitcode"))
            exit_code = exit_code_marker[0].args[0] if exit_code_marker else 0
            item._obj = cunit(
                *item._obj.__cunit__, full_name=item.name, exit_code=exit_code
            )
