import os
import platform

PY3_LATEST = 12

try:
    REQUESTED_PYTHON_VERSIONS = [
        tuple(int(_) for _ in v.split("."))
        for v in os.getenv("AUSTIN_TESTS_PYTHON_VERSIONS", "").split(",")
    ]
except Exception:
    REQUESTED_PYTHON_VERSIONS = None


match platform.system():
    case "Darwin":
        PYTHON_VERSIONS = REQUESTED_PYTHON_VERSIONS or [
            (3, _) for _ in range(8, PY3_LATEST + 1)
        ]
    case _:
        PYTHON_VERSIONS = REQUESTED_PYTHON_VERSIONS or [
            (3, _) for _ in range(8, PY3_LATEST + 1)
        ]
