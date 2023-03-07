import sys
from itertools import product
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import tarfile
from io import BytesIO
from test.utils import Variant
from urllib.error import HTTPError
from urllib.request import urlopen
import json


def get_latest_release() -> str:
    with urlopen(
        "https://api.github.com/repos/p403n1x87/austin/releases/latest"
    ) as stream:
        return json.loads(stream.read().decode("utf-8"))["tag_name"].strip("v")


def download_release(version: str, dest: Path, variant_name: str = "austin") -> Variant:
    if version == "dev":
        return Variant(f"src/{variant_name}")

    binary_dest = dest / version
    binary = binary_dest / variant_name

    if not binary.exists():
        prefix = "https://github.com/p403n1x87/austin/releases/download/"
        for flavour, v in product({"-gnu", ""}, {"", "v"}):
            try:
                with urlopen(
                    f"{prefix}v{version}/{variant_name}-{v}{version}{flavour}-linux-amd64.tar.xz"
                ) as stream:
                    buffer = BytesIO(stream.read())
                    binary_dest.mkdir(parents=True, exist_ok=True)
                    tar = tarfile.open(fileobj=buffer, mode="r:xz")
                    tar.extract(variant_name, str(binary_dest))
            except HTTPError:
                continue
            break
        else:
            raise RuntimeError(f"Could not download Austin version {version}")

    variant = Variant(str(binary))

    out = variant("-V").stdout
    assert f"{variant_name} {version}" in out, (f"{variant_name} {version}", out)

    return variant


def download_latest(dest: Path, variant_name: str = "austin") -> Variant:
    return download_release(get_latest_release(), dest, variant_name)


def get_dev(variant_name: str = "austin") -> Variant:
    return download_release("dev", None, variant_name)
