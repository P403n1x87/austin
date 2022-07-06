from pathlib import Path

HERE = Path(__file__).parent


def get_current_version_from_changelog() -> str:
    with (HERE.parent / "ChangeLog").open() as cl:
        for line in cl.readlines():
            if not line or line.startswith(" "):
                continue
            _, _, version = line.partition(" ")
            if version[0] == "v":
                return version[1:].strip()
