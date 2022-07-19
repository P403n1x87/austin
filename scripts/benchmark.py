# Run as python3 scripts/benchmark.py from the repository root directory.

import re
import sys
from argparse import ArgumentParser
from math import floor, log
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent))

import tarfile
from io import BytesIO
from test.utils import Variant, metadata, target
from urllib.error import HTTPError
from urllib.request import urlopen

VERSIONS = ("3.0.0", "3.1.0", "3.2.0", "3.3.0", "dev")
SCENARIOS = [
    *[
        (
            f"Wall time [sampling interval: {i}]",
            ["-Pi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            f"CPU time [sampling interval: {i}]",
            ["-Psi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            f"Full metrics [sampling interval: {i}]",
            ["-Pfi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
]


def get_stats(output: str) -> dict:
    meta = metadata(output)
    raw_saturation = meta["saturation"]
    _, _, raw_samples = raw_saturation.partition("/")

    duration = float(meta["duration"]) / 1e6
    samples = int(raw_samples)
    saturation = eval(raw_saturation)
    error_rate = eval(meta["errors"])
    sampling = int(meta["sampling"].split(",")[1])

    return {
        "Sample Rate": samples / duration,
        "Saturation": saturation,
        "Error Rate": error_rate,
        "Sampling Speed": sampling,
    }


def download_release(version: str, dest: Path) -> Variant:
    if version == "dev":
        return Variant("src/austin")

    binary_dest = dest / version
    binary = binary_dest / "austin"

    if not binary.exists():
        prefix = "https://github.com/p403n1x87/austin/releases/download/"
        for variant in {"-gnu", ""}:
            try:
                with urlopen(
                    f"{prefix}v{version}/austin-{version}{variant}-linux-amd64.tar.xz"
                ) as stream:
                    buffer = BytesIO(stream.read())
                    binary_dest.mkdir(parents=True, exist_ok=True)
                    tar = tarfile.open(fileobj=buffer, mode="r:xz")
                    tar.extract("austin", str(binary_dest))
            except HTTPError:
                continue
            break
        else:
            raise RuntimeError(f"Could not download Austin version {version}")

    variant = Variant(str(binary))

    assert f"austin {version}" in variant("-V").stdout

    return variant


class Outcome:
    def __init__(self, data: list[float]) -> None:
        self.mean = sum(data) / len(data)
        self.stdev = (
            sum(((v - self.mean) ** 2 for v in data)) / (len(data) - 1)
        ) ** 0.5

    def __repr__(self):
        n = -floor(log(self.stdev, 10)) if self.stdev else 0
        rmean = round(self.mean, n)
        rstdev = round(self.stdev, n)
        if n <= 0:
            rmean = int(rmean)
            rstdev = int(rstdev)

        return f"{rmean} ± {rstdev}"

    __str__ = __repr__

    def __len__(self):
        return len(repr(self))


def render(table):
    _, row = table[0]
    cols = list(row.keys())
    max_vh = max(len(e[0]) for e in table)

    col_widths = [max(max(len(r[col]), len(col)) for _, r in table) for col in cols]
    div_len = sum(col_widths) + (len(cols) + 1) * 2 + max_vh

    print("=" * div_len)
    print(
        (" " * (max_vh + 2))
        + "".join(f"{col:^{cw+2}}" for col, cw in zip(cols, col_widths))
    )
    print("-" * div_len)

    for v, row in table:
        print(f"{v:^{max_vh+2}}", end="")
        for col, cw in zip(cols, col_widths):
            print(f"{str(row[col]):^{cw+2}}", end="")
        print()

    print("=" * div_len)


def main():
    argp = ArgumentParser()
    argp.add_argument(
        "-k",
        type=re.compile,
        help="Run benchmark scenarios that match the given regular expression",
    )

    opts = argp.parse_args()

    print(
        f"Running Austin benchmarks with Python {'.'.join(str(_) for _ in sys.version_info[:3])}",
        end="\n\n",
    )

    for title, args in SCENARIOS:
        if opts.k is not None and not opts.k.match(title):
            continue

        print(title)

        table = []
        for version in VERSIONS:
            print(f"> Running with Austin {version} ...    ", end="\r")
            austin = download_release(version, Path("/tmp"))

            stats = [get_stats(austin(*args).stdout) for _ in range(10)]
            table.append(
                (
                    version,
                    {
                        key: Outcome([s[key] for s in stats])
                        for key in list(stats[0].keys())
                    },
                )
            )

        render(table)
        print()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye!")
