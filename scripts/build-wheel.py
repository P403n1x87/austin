from argparse import ArgumentParser
from io import BytesIO, StringIO
import json
from pathlib import Path
import tarfile
from urllib.error import HTTPError
from urllib.request import urlopen
from wheel.wheelfile import WheelFile
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

METADATA = {
    "Summary": "Austin - Frame Stack Sampler for CPython",
    "Author": "Gabriele N. Tornetta",
    "License": "GPLv3+",
    "Classifier": [
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: GNU General Public License v3 or later (GPLv3+)",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
    ],
    "Project-URL": [
        "Homepage, https://github.com/P403n1x87/austin",
        "Source Code, https://github.com/P403n1x87/austin",
        "Bug Tracker, https://github.com/P403n1x87/austin/issues",
        "Changelog, https://github.com/P403n1x87/austin/blob/master/ChangeLog",
        "Documentation, https://github.com/P403n1x87/austin/blob/master/README.md",
        "Funding, https://github.com/sponsors/P403n1x87",
        "Release Notes, https://github.com/P403n1x87/austin/releases",
    ],
    "Description-Content-Type": "text/markdown",
}


AUSTIN_WHEELS = {
    "manylinux_2_17_aarch64.manylinux2014_aarch64": (
        "gnu-linux-aarch64.tar.xz",
        ("austin", "austinp"),
    ),
    "manylinux_2_12_x86_64.manylinux2010_x86_64": (
        "gnu-linux-amd64.tar.xz",
        ("austin", "austinp"),
    ),
    "manylinux_2_17_armv7l.manylinux2014_armv7l": (
        "gnu-linux-armv7.tar.xz",
        ("austin", "austinp"),
    ),
    "manylinux_2_17_ppc64le.manylinux2014_ppc64le": (
        "gnu-linux-ppc64le.tar.xz",
        ("austin", "austinp"),
    ),
    "macosx_11_0_x86_64": ("mac64.zip", ("austin",)),
    "macosx_11_0_arm64": ("mac-arm64.zip", ("austin",)),
    "musllinux_1_1_aarch64": ("musl-linux-aarch64.tar.xz", ("austin",)),
    "musllinux_1_1_x86_64": ("musl-linux-amd64.tar.xz", ("austin",)),
    "musllinux_1_1_armv7l": ("musl-linux-armv7.tar.xz", ("austin",)),
    "musllinux_1_1_ppc64le": ("musl-linux-ppc64le.tar.xz", ("austin",)),
    "win_amd64": ("win64.zip", ("austin",)),
}


def make_message(headers, payload=None):
    message = StringIO()

    for name, value in headers.items():
        if isinstance(value, list):
            for value_part in value:
                print(f"{name}: {value_part}", file=message)
        else:
            print(f"{name}: {value}", file=message)

    if payload:
        print(file=message)
        print(payload, file=message)

    return message.getvalue().encode("utf-8")


def write_austin_wheel(out_dir, *, version, platform, austin_bin_data):
    package_name = "austin-dist"
    python = ".".join(("py2", "py3"))
    dist_name = package_name.replace("-", "_")
    wheel_name = f"{dist_name}-{version}-{python}-none-{platform}.whl"
    dist_info = f"{dist_name}-{version}.dist-info"
    wheel_path = out_dir / wheel_name

    print(f"Building {wheel_name} ... ", end="", flush=True)

    contents = {}

    for binary_name, binary_data in austin_bin_data:
        zip_info = ZipInfo(f"{dist_name}-{version}.data/scripts/{binary_name}")
        zip_info.external_attr |= 33261 << 16
        contents[zip_info] = binary_data

    contents[f"{dist_info}/METADATA"] = make_message(
        {
            "Metadata-Version": "2.1",
            "Name": package_name,
            "Version": version,
            **METADATA,
        },
        Path("README.md").read_text("utf-8"),
    )
    contents[f"{dist_info}/WHEEL"] = make_message(
        {
            "Wheel-Version": "1.0",
            "Generator": "austin-dist build-wheel.py",
            "Root-Is-Purelib": "false",
            "Tag": f"{python}-none-{platform}",
        }
    )

    with WheelFile(str(wheel_path), "w") as wheel:
        for member_info, member_source in contents.items():
            if not isinstance(member_info, ZipInfo):
                member_info = ZipInfo(member_info)
                member_info.external_attr = 0o644 << 16
            member_info.file_size = len(member_source)
            member_info.compress_type = ZIP_DEFLATED
            wheel.writestr(member_info, bytes(member_source))

    assert wheel_path.exists(), "wheel file created"

    print("done")


def get_latest_release() -> str:
    with urlopen(
        "https://api.github.com/repos/p403n1x87/austin/releases/latest"
    ) as stream:
        return json.loads(stream.read().decode("utf-8"))["tag_name"].strip("v")


def download_release(
    version: str, suffix: str, variant_name: str = "austin"
) -> tuple[str, bytes]:
    prefix = "https://github.com/p403n1x87/austin/releases/download/"
    try:
        with urlopen(f"{prefix}v{version}/{variant_name}-{version}-{suffix}") as stream:
            buffer = BytesIO(stream.read())
            if suffix.endswith(".tar.xz"):
                with tarfile.open(fileobj=buffer, mode="r:xz") as tar:
                    return variant_name, tar.extractfile(variant_name).read()
            elif suffix.endswith(".zip"):
                with ZipFile(buffer) as zip:
                    try:
                        return variant_name, zip.read(variant_name)
                    except KeyError:
                        file_name = f"{variant_name}.exe"
                        return file_name, zip.read(file_name)
            raise ValueError(f"Unknown archive extension: {suffix}")
    except HTTPError:
        raise RuntimeError(f"Could not download Austin version {version}")


if __name__ == "__main__":
    argp = ArgumentParser()

    argp.add_argument(
        "--version",
        help="Austin version to build wheels for",
        default=None,
    )

    argp.add_argument(
        "--platform",
        help="Platform to build wheels for",
        choices=AUSTIN_WHEELS.keys(),
        default=None,
    )

    argp.add_argument(
        "--files",
        help="The variant to binary file mapping (e.g. austin:src/austin)",
        nargs="+",
        type=str,
        default=None,
    )

    args = argp.parse_args()

    version = args.version or get_latest_release()

    dist_dir = Path.cwd() / "dist"
    dist_dir.mkdir(exist_ok=True)

    for platform, (suffix, variants) in AUSTIN_WHEELS.items():
        if args.platform is not None and args.platform != platform:
            continue

        bin_data = (
            (
                download_release(version, suffix, variant_name=variant)
                for variant in variants
            )
            if args.files is None
            else (
                (file_name, Path(bin_path).read_bytes())
                for file_name, bin_path in (file.split(":") for file in args.files)
            )
        )

        write_austin_wheel(
            dist_dir,
            version=version,
            platform=platform,
            austin_bin_data=bin_data,
        )
