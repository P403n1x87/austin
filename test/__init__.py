import os
import platform

PY3_EARLIEST = 10
PY3_LATEST = 15


def _ver_tuple(v: str) -> tuple[int, ...]:
    """Parse '3.13' or '3.13t' into a numeric tuple for comparison."""
    return tuple(int(x.rstrip("t")) for x in v.split("."))


try:
    raw = os.getenv("AUSTIN_TESTS_PYTHON_VERSIONS", "").split(",")
    REQUESTED_PYTHON_VERSIONS = [v.strip() for v in raw if v.strip()] or None
except Exception:
    REQUESTED_PYTHON_VERSIONS = None


match platform.system():
    case "Darwin":
        PYTHON_VERSIONS = REQUESTED_PYTHON_VERSIONS or [
            f"3.{_}" for _ in range(PY3_EARLIEST, PY3_LATEST + 1)
        ]
    case _:
        PYTHON_VERSIONS = REQUESTED_PYTHON_VERSIONS or [
            f"3.{_}" for _ in range(PY3_EARLIEST, PY3_LATEST + 1)
        ]
