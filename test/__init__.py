import platform

PY3_LATEST = 10

match platform.system():
    case "Darwin":
        PYTHON_VERSIONS = [(3, _) for _ in range(7, PY3_LATEST + 1)]
    case _:
        PYTHON_VERSIONS = [(2, 7)] + [(3, _) for _ in range(5, PY3_LATEST + 1)]
