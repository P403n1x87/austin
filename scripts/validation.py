# Run as python3 scripts/validation.py from the repository root directory.
# Ensure dependencies from requirements-val.txt are installed.

import codecs
import os
import re
import sys
import typing as t
from argparse import ArgumentParser
from dataclasses import dataclass
from pathlib import Path

import common
from stats import AustinFlameGraph, compare

from test.utils import python, target


def tee(data: bytes, output: str) -> bytes:
    Path(output.replace(" ", "_").replace(".", "_").lower()).with_suffix(
        ".mojo"
    ).write_bytes(data)
    return data


@dataclass
class Scenario:
    title: str
    variant: str
    args: tuple[str, ...]

    def run(
        self, austin: common.VersionedVariant, n: int = 10
    ) -> t.List[AustinFlameGraph]:
        try:
            return [
                AustinFlameGraph.from_mojo(
                    tee(
                        austin(
                            *scenario.args,
                            convert=False,
                        ).stdout,
                        f"{scenario.title}-{austin.version}-{i}",
                    )
                )
                for i in range(n)
            ]
        except Exception as e:
            msg = f"Error running scenario {self.title} with {austin} and arguments {self.args}: {e}"
            raise RuntimeError(msg) from e


if (PYTHON_VERSION := os.getenv("AUSTIN_TESTS_PYTHON_VERSIONS")) is None:
    major, minor = sys.version_info[:2]
    PYTHON_VERSION = f"{major}.{minor}"

PYTHON = python(PYTHON_VERSION)


SCENARIOS = [
    Scenario(
        "Wall time",
        "austin",
        (
            "-i",
            "500",
            *PYTHON,
            target("target34.py"),
        ),
    ),
    Scenario(
        "CPU time",
        "austin",
        (
            "-ci",
            "500",
            *PYTHON,
            target("target34.py"),
        ),
    ),
    Scenario(
        "Wall time [multiprocessing]",
        "austin",
        (
            "-Ci",
            "500",
            *PYTHON,
            target("target_mp.py"),
        ),
    ),
    Scenario(
        "CPU time [multiprocessing]",
        "austin",
        (
            "-Cci",
            "500",
            *PYTHON,
            target("target_mp.py"),
        ),
    ),
]


def validate(scenario: Scenario, runs: int = 10) -> float:
    return compare(
        # Base branch version
        x=scenario.run(common.get_base(variant_name=scenario.variant), runs),
        # Development version
        y=scenario.run(common.get_dev(variant_name=scenario.variant), runs),
        # Keep only the stacks that are present in all runs
        threshold=runs,
    )


def generate_markdown_report(
    failures: t.List[tuple[Scenario, float]], path: Path
) -> None:
    output = f"### Python {PYTHON_VERSION}\n\n"

    if not failures:
        output += "✨ All scenarios validated successfully! ✨"
    else:
        output += "🔴 The following scenarios did not pass data validation:\n\n"
        output += "| Scenario | p-value |\n"
        output += "|----------|---------|\n"
        for scenario, p in failures:
            output += f"| {scenario.title} | {p:.2%} |\n"

    path.write_text(output)


if __name__ == "__main__":
    argp = ArgumentParser()

    argp.add_argument(
        "-k",
        type=re.compile,
        help="Run data validation scenarios that match the given regular expression",
    )

    argp.add_argument(
        "-n",
        type=int,
        default=30,
        help="Number of profiles to collect",
    )

    argp.add_argument(
        "-i",
        "--ignore-errors",
        action="store_true",
        help="Ignore encoding errors",
    )

    argp.add_argument(
        "-p",
        "--p-value",
        type=float,
        default=0.01,
        help="p-value threshold",
    )

    argp.add_argument(
        "-r",
        "--report",
        type=Path,
        help="Path to store the validation report",
    )

    opts = argp.parse_args()

    if opts.ignore_errors:
        codecs.register_error("strict", codecs.ignore_errors)

    print("# Austin Data Validation\n")

    failures: t.List[tuple[Scenario, float]] = []
    for scenario in SCENARIOS:
        print(f"Validating {scenario.title} ...", flush=True, file=sys.stderr, end=" ")

        result_icon = "✅"
        if (p := validate(scenario, runs=opts.n)) < opts.p_value:
            failures.append((scenario, p))
            result_icon = "❌"

        print(result_icon, file=sys.stderr)

    if opts.report:
        generate_markdown_report(failures, opts.report)

    if failures:
        print("💥 The following scenarios failed to validate:\n")

        for scenario, p in failures:
            print(f"- {scenario.title} [{p:.2%}]")

        exit(1)

    print(f"✨ 🍰 ✨ All {len(SCENARIOS)} scenarios validated successfully!")
