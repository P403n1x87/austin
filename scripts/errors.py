from dataclasses import dataclass
from pathlib import Path


@dataclass
class AustinError:
    alias: str
    message: str
    fatal: bool = False


austin_error_declarations = [
    AustinError(
        "OK",
        "No error",
    ),
    # ---- Platform errors ----
    AustinError(
        "OS",
        "Operating system error",
        fatal=True,
    ),
    AustinError(
        "PERM",
        "Permission error",
        fatal=True,
    ),
    AustinError(
        "MEMCOPY",
        "Memory copy error",
        fatal=True,
    ),
    AustinError(
        "MALLOC",
        "Memory allocation error",
        fatal=True,
    ),
    AustinError(
        "IO",
        "I/O error",
        fatal=True,
    ),
    # ---- Input errors ----
    AustinError(
        "CMDLINE",
        "Command line error",
        fatal=True,
    ),
    AustinError(
        "ENV",
        "Environment error",
        fatal=True,
    ),
    # ---- Value errors ----
    AustinError(
        "VALUE",
        "Value error",
    ),
    AustinError(
        "NULL",
        "Null pointer error",
        fatal=True,
    ),
    AustinError(
        "VERSION",
        "Python version error",
        fatal=True,
    ),
    AustinError(
        "BINARY",
        "Binary analysis error",
    ),
    AustinError(
        "PYOBJECT",
        "Python object error",
    ),
    AustinError(
        "VM",
        "VM maps error",
    ),
    # ---- Control errors ----
    AustinError(
        "ITEREND",
        "Iteration ended error",
    ),
]

max_error = len(austin_error_declarations)
error_info_table = (
    "\n"
    + "\n".join(
        f'{{"{error.message}", {str(error.fatal).lower()}}}, /* {error.alias} */'
        for error in austin_error_declarations
    )
    + "\n"
)
error_alias_decls = "\n".join(
    f"#define AUSTIN_E{error.alias} {i}"
    for i, error in enumerate(austin_error_declarations)
)
error_python_enum = "\n".join(
    f"    {error.alias} = {i}" for i, error in enumerate(austin_error_declarations)
)
error_python_messages = "\n".join(
    f'        {i}: "{error.message}",'
    for i, error in enumerate(austin_error_declarations)
)
error_python_fatals = "\n".join(
    f"        {i}: {str(error.fatal)},"
    for i, error in enumerate(austin_error_declarations)
)

HERE = Path(__file__).parent
SRC = HERE.parent / "src"
TEST = HERE.parent / "test"


def render(src: Path, placeholders: dict) -> None:
    text = src.with_suffix(f"{src.suffix}.template").read_text()

    for p, v in placeholders.items():
        text = text.replace(f"[[{p}]]", str(v))

    src.write_text(text)


render(SRC / "error.c", {"max_error": max_error, "error_info_table": error_info_table})
render(SRC / "error.h", {"error_alias_decls": error_alias_decls})
render(
    TEST / "error.py",
    {
        "errors": error_python_enum,
        "error_messages": error_python_messages,
        "error_fatals": error_python_fatals,
    },
)
