# Run as python3 scripts/validation.py from the repository root directory.
# Ensure dependencies from requirements-val.txt are installed.
#
# The `--merge` mode uses only the standard library, so it can run in a
# reporting job without the heavy validation dependencies installed.

import codecs
import os
import re
import sys
import typing as t
from argparse import ArgumentParser
from dataclasses import dataclass
from pathlib import Path

# Make the repo root importable so `test.utils` resolves when the script is
# invoked as `python scripts/validation.py` from the repo root.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")


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

    def run(self, austin, n: int = 10):
        from stats import AustinFlameGraph

        results = []
        for i in range(n):
            try:
                data = austin(*self.args, convert=False).stdout
                if data:
                    results.append(
                        AustinFlameGraph.from_mojo(
                            tee(data, f"{self.title}-{austin.version}-{i}")
                        )
                    )
            except Exception as e:
                print(
                    f"WARNING: run {i} of scenario {self.title!r} with {austin} "
                    f"failed: {e}",
                    file=sys.stderr,
                )
        return results


_NOT_APPLICABLE = object()


def validate(scenario: Scenario, runs: int = 10) -> t.Union[float, object]:
    import common
    from stats import compare

    base_results = scenario.run(common.get_base(variant_name=scenario.variant), runs)
    dev_results = scenario.run(common.get_dev(variant_name=scenario.variant), runs)

    if not base_results:
        # We might be testing a new feature that is not available from the base
        # version. We still collect dev data above so that the .mojo profiles
        # are available as artifacts for manual validation.
        return _NOT_APPLICABLE

    # threshold=runs keeps only the stacks that are present in all runs
    return compare(x=base_results, y=dev_results, threshold=runs)


def generate_markdown_report(
    python_version: str,
    failures: t.List[t.Tuple[Scenario, float]],
    skipped: t.List[Scenario],
    path: Path,
) -> None:
    output = f"### Python {python_version}\n\n"

    if not failures:
        output += "✨ All scenarios validated successfully! ✨"
    else:
        output += "🔴 The following scenarios did not pass data validation:\n\n"
        output += "| Scenario | p-value |\n"
        output += "|----------|---------|\n"
        for scenario, p in failures:
            output += f"| {scenario.title} | {p:.2%} |\n"

    if skipped:
        output += (
            "\n\n⚪ The following scenarios were skipped"
            " (base produced no data; PR profiles collected for manual validation):\n\n"
        )
        for scenario in skipped:
            output += f"- {scenario.title}\n"

    path.write_text(output, encoding="utf-8")


# ---------------------------------------------------------------------------
# Merge mode — stdlib only
# ---------------------------------------------------------------------------

_RESULT_FILENAME_RE = re.compile(
    r"^results-(?P<platform>[^-]+)-(?P<arch>[^-]+)-(?P<python>[^-]+)$"
)


def _parse_result_filename(path: Path) -> t.Tuple[str, str, str]:
    m = _RESULT_FILENAME_RE.match(path.stem)
    if m is None:
        raise ValueError(
            f"unexpected result filename {path.name!r}; expected "
            f"results-{{platform}}-{{arch}}-{{python_version}}.txt"
        )
    return m["platform"], m["arch"], m["python"]


def _version_key(v: str) -> t.Tuple[int, ...]:
    return tuple(int(p) for p in re.findall(r"\d+", v))


def merge_reports(files: t.Iterable[Path], output: Path) -> None:
    """Combine per-job validation reports into a single markdown report
    grouped by platform + architecture.

    Each input file is expected to be named
    ``results-{platform}-{arch}-{python_version}.txt`` and to contain a
    ``### Python X.Y`` heading at the top (as emitted by
    :func:`generate_markdown_report`).
    """
    groups: t.Dict[t.Tuple[str, str], t.List[t.Tuple[str, Path]]] = {}
    for f in files:
        platform, arch, python_version = _parse_result_filename(f)
        groups.setdefault((platform, arch), []).append((python_version, f))

    parts: t.List[str] = ["## Austin Data Validation\n"]
    for platform, arch in sorted(groups):
        entries = sorted(groups[(platform, arch)], key=lambda e: _version_key(e[0]))
        parts.append(f"\n### {platform.capitalize()} ({arch})\n")
        for _, path in entries:
            # Demote the per-Python-version headings emitted by
            # generate_markdown_report so they nest under the platform/arch
            # section.
            content = re.sub(r"(?m)^### ", "#### ", path.read_text(encoding="utf-8"))
            parts.append("\n" + content.rstrip() + "\n")

    output.write_text("".join(parts), encoding="utf-8")


# ---------------------------------------------------------------------------
# Run mode
# ---------------------------------------------------------------------------


def _build_scenarios(python_version: str) -> t.List[Scenario]:
    from test.utils import python, target

    py = python(python_version)

    return [
        Scenario(
            "Wall time",
            "austin",
            ("-i", "500", *py, target("target34.py")),
        ),
        Scenario(
            "CPU time",
            "austin",
            ("-ci", "500", *py, target("target34.py")),
        ),
        Scenario(
            "Wall time [multiprocessing]",
            "austin",
            ("-Ci", "500", *py, target("target_mp.py")),
        ),
        Scenario(
            "CPU time [multiprocessing]",
            "austin",
            ("-Cci", "500", *py, target("target_mp.py")),
        ),
        Scenario(
            "Native wall time",
            "austin",
            ("-ni", "1ms", *py, target("target34.py")),
        ),
        Scenario(
            "Native CPU time",
            "austin",
            ("-nci", "1ms", *py, target("target34.py")),
        ),
        Scenario(
            "Wall time [asyncio]",
            "austin",
            ("-i", "500", *py, target("target_asyncio.py")),
        ),
        Scenario(
            "CPU time [asyncio]",
            "austin",
            ("-ci", "500", *py, target("target_asyncio_cpu.py")),
        ),
        Scenario(
            "Wall time [asyncio multiloop]",
            "austin",
            ("-i", "500", *py, target("target_asyncio_multiloop.py")),
        ),
        Scenario(
            "CPU time [asyncio multiloop]",
            "austin",
            ("-ci", "500", *py, target("target_asyncio_multiloop.py")),
        ),
    ]


def main(opts) -> None:
    python_version = os.getenv("AUSTIN_TESTS_PYTHON_VERSIONS")
    if python_version is None:
        major, minor = sys.version_info[:2]
        python_version = f"{major}.{minor}"

    scenarios = _build_scenarios(python_version)

    print("# Austin Data Validation\n")

    failures: t.List[t.Tuple[Scenario, float]] = []
    skipped: t.List[Scenario] = []
    for scenario in scenarios:
        if opts.k is not None and not opts.k.search(scenario.title):
            continue

        print(f"Validating {scenario.title} ...", flush=True, file=sys.stderr, end=" ")

        p = validate(scenario, runs=opts.n)
        if p is _NOT_APPLICABLE:
            skipped.append(scenario)
            print(
                "⚪ (not applicable — base produced no data; PR profiles collected)",
                file=sys.stderr,
            )
            continue

        result_icon = "✅"
        if p < opts.p_value:
            failures.append((scenario, p))
            result_icon = "❌"

        print(result_icon, file=sys.stderr)

    if opts.report:
        generate_markdown_report(python_version, failures, skipped, opts.report)

    if failures:
        print("💥 The following scenarios failed to validate:\n")

        for scenario, p in failures:
            print(f"- {scenario.title} [{p:.2%}]")

        exit(1)

    print(f"✨ 🍰 ✨ All {len(scenarios)} scenarios validated successfully!")


if __name__ == "__main__":
    argp = ArgumentParser()

    argp.add_argument(
        "--merge",
        nargs="+",
        type=Path,
        metavar="FILE",
        default=None,
        help="Merge per-job validation reports (produced with --report) into a "
        "single markdown report grouped by platform + architecture. Expects "
        "input files named results-{platform}-{arch}-{python_version}.txt.",
    )

    argp.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output path for --merge mode.",
    )

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
        default=0.001,
        help="p-value threshold",
    )

    argp.add_argument(
        "-r",
        "--report",
        type=Path,
        help="Path to store the validation report",
    )

    opts = argp.parse_args()

    if opts.merge is not None:
        if opts.output is None:
            argp.error("--merge requires --output")
        merge_reports(opts.merge, opts.output)
        sys.exit(0)

    if opts.ignore_errors:
        codecs.register_error("strict", codecs.ignore_errors)

    main(opts)
