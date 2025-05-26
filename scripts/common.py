import platform
import sys
import typing as t
import zipfile
from itertools import product
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import json
import tarfile
from io import BytesIO
from urllib.error import HTTPError
from urllib.request import urlopen

from test.utils import Variant

if sys.platform.startswith("win32"):
    RELEASE_ARCH_SUFFIX = "-win64.zip"
elif sys.platform.startswith("darwin"):
    if platform.machine == "arm64":
        RELEASE_ARCH_SUFFIX = "mac-arm64.zip"
    else:
        RELEASE_ARCH_SUFFIX = "mac64.zip"
else:
    RELEASE_ARCH_SUFFIX = "linux-amd64.tar.xz"


class VersionedVariant(Variant):
    def __init__(self, name: str, version: str) -> None:
        super().__init__(name)
        self.version = version


def get_latest_release() -> str:
    with urlopen(
        "https://api.github.com/repos/p403n1x87/austin/releases/latest"
    ) as stream:
        return json.loads(stream.read().decode("utf-8"))["tag_name"].strip("v")


def download_release(
    version: str, dest: t.Optional[Path], variant_name: str = "austin"
) -> VersionedVariant:
    if version == "dev":
        return VersionedVariant(f"src/{variant_name}", version)

    if dest is None:
        msg = "Destination path must be provided for non-dev versions"
        raise ValueError(msg)

    binary_dest = dest / version
    binary = binary_dest / variant_name

    if not binary.exists():
        prefix = "https://github.com/p403n1x87/austin/releases/download/"
        for flavour, v in product({"-gnu", ""}, {"", "v"}):
            try:
                with urlopen(
                    f"{prefix}v{version}/{variant_name}-{v}{version}{flavour}-{RELEASE_ARCH_SUFFIX}"
                ) as stream:
                    buffer = BytesIO(stream.read())
                    binary_dest.mkdir(parents=True, exist_ok=True)
                    if RELEASE_ARCH_SUFFIX.endswith(".zip"):
                        with zipfile.ZipFile(buffer) as zip_file:
                            zip_file.extract(variant_name, str(binary_dest))
                    else:
                        tar = tarfile.open(fileobj=buffer, mode="r:xz")
                        tar.extract(variant_name, str(binary_dest))
            except HTTPError:
                continue
            break
        else:
            raise RuntimeError(f"Could not download Austin version {version}")

    variant = VersionedVariant(str(binary), version)

    out = variant("-V").stdout
    assert f"{variant_name} {version}" in out, (f"{variant_name} {version}", out)

    return variant


def download_latest(dest: Path, variant_name: str = "austin") -> VersionedVariant:
    return download_release(get_latest_release(), dest, variant_name)


def get_dev(variant_name: str = "austin") -> VersionedVariant:
    return download_release("dev", None, variant_name)
