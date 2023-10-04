# Run as python3 scripts/benchmark.py from the repository root directory.
# Ensure dependencies from requirements-bm.txt are installed.

import abc
import re
import sys
import typing as t
from argparse import ArgumentParser
from math import floor, log
from pathlib import Path
from test.utils import metadata, target
from textwrap import wrap

from common import download_release
from scipy.stats import ttest_ind

VERSIONS = ("3.4.1", "3.5.0", "dev")
SCENARIOS = [
    *[
        (
            "austin",
            f"Wall time [sampling interval: {i}]",
            ["-Pi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            "austin",
            f"CPU time [sampling interval: {i}]",
            ["-Psi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            "austin",
            f"RSA keygen [sampling interval: {i}]",
            ["-Psi", str(i), sys.executable, "-m", "test.bm.rsa_key_generator"],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            "austin",
            f"Full metrics [sampling interval: {i}]",
            ["-Pfi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        (
            "austin",
            f"Multiprocess wall time [sampling interval: {i}]",
            ["-CPfi", str(i), sys.executable, target("target_mp.py"), "16"],
        )
        for i in (1, 10, 100, 1000)
    ],
]


# The metrics we evaluate and whether they are to be maximised or minimised.
METRICS = [
    ("Sample Rate", +1),
    ("Saturation", -1),
    ("Error Rate", -1),
    ("Sampling Speed", -1),
]


def get_stats(output: str) -> t.Optional[dict]:
    try:
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

    except Exception:
        # Failed to get stats
        return None


class Outcome:
    __critical_p__ = 0.025

    def __init__(self, data: list[float]) -> None:
        self.data = data
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

    def __eq__(self, other: "Outcome") -> bool:
        t, p = ttest_ind(self.data, other.data, equal_var=False)
        return p < self.__critical_p__


Results = t.Tuple[str, t.Dict[str, Outcome]]


class Renderer(abc.ABC):
    BETTER = "better"
    WORSE = "worse"
    SAME = "same"

    @abc.abstractmethod
    def render_header(self, title: str, level: int = 1) -> str:
        ...

    @abc.abstractmethod
    def render_paragraph(self, text: str) -> str:
        ...

    @abc.abstractmethod
    def render_table(self, table) -> str:
        ...

    @abc.abstractmethod
    def render_scenario(
        self, title, results: t.List[t.Tuple[str, t.List[Results]]]
    ) -> str:
        ...

    @abc.abstractmethod
    def render_summary(
        self, summary: t.List[t.Tuple[str, t.List[t.Tuple[str, bool, int]]]]
    ) -> str:
        ...

    def render_scenario(
        self, title, table: t.List[t.Tuple[str, t.List[Results]]]
    ) -> str:
        self.render_header(title, level=2)
        self.render_table(table)
        print()

    def render_summary(self, summary):
        self.render_header("Benchmark Summary", level=2)
        self.render_paragraph(f"Comparison of {VERSIONS[-1]} against {VERSIONS[-2]}.")

        if not summary:
            self.render_paragraph(
                "No significant difference in performance between versions."
            )
            return

        self.render_paragraph(
            "The following scenarios show a statistically significant difference "
            "in performance between the two versions."
        )

        self.render_table(
            [
                (
                    title,
                    {
                        m: {1: self.BETTER, -1: self.WORSE}[s] if c else self.SAME
                        for m, c, s in tests
                    },
                )
                for title, tests in summary
            ]
        )


class TerminalRenderer(Renderer):
    def render_table(self, table: t.List[t.Tuple[str, t.List[Results]]]) -> str:
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

    def render_header(self, title: str, level: int = 1) -> str:
        print(title)
        print({1: "=", 2: "-", 3: "~"}.get(level, "-") * len(title))
        print()

    def render_paragraph(self, text: str) -> str:
        for _ in wrap(text):
            print(_)
        print()


class MarkdownRenderer(Renderer):
    BETTER = ":green_circle:"
    WORSE = ":red_circle:"
    SAME = ":yellow_circle:"

    def render_header(self, title: str, level: int = 1) -> str:
        print(f"{'#' * level} {title}")
        print()

    def render_paragraph(self, text: str) -> str:
        print(text)
        print()

    def render_table(self, table: t.List[t.Tuple[str, t.List[Results]]]) -> str:
        _, row = table[0]
        cols = list(row.keys())
        max_vh = max(len(e[0]) for e in table)

        col_widths = [max(max(len(r[col]), len(col)) for _, r in table) for col in cols]
        div_len = sum(col_widths) + (len(cols) + 1) * 2 + max_vh

        print("|     |" + "|".join(f" {col} " for col in cols) + "|")
        print("| --- |" + "|".join(f":{'-' * len(col)}:" for col in cols) + "|")

        for v, row in table:
            print(
                f"| {v} |"
                + "|".join(
                    f" {str(row[col]):^{cw}} " for col, cw in zip(cols, col_widths)
                )
                + "|"
            )

    def render_scenario(
        self, title, table: t.List[t.Tuple[str, t.List[Results]]]
    ) -> str:
        print("<details>")
        print(f"<summary><strong>{title}</strong></summary>")
        print()
        super().render_scenario(title, table)
        print("</details>")
        print()


def summarize(results: t.List[t.Tuple[str, t.List[Results]]]):
    summary = []
    for title, table in results:
        (_, a), (_, b) = table[-2:]
        tests = [
            (
                m,
                a[m] == b[m],
                int((b[m].mean - a[m].mean) * s / (abs(b[m].mean - a[m].mean) or 1)),
            )
            for m, s in METRICS
        ]
        if any(c for _, c, _ in tests):
            summary.append((title, tests))
    return summary


def main():
    argp = ArgumentParser()

    argp.add_argument(
        "-k",
        type=re.compile,
        help="Run benchmark scenarios that match the given regular expression",
    )

    argp.add_argument(
        "-n",
        type=int,
        default=10,
        help="Number of times to run each scenario",
    )

    argp.add_argument(
        "-f",
        "--format",
        type=str,
        choices=["terminal", "markdown"],
        default="terminal",
        help="The output format",
    )

    argp.add_argument(
        "-l",
        "--last",
        action="store_true",
        help="Run only with the last release of Austin",
    )

    argp.add_argument(
        "-p",
        "--pvalue",
        type=float,
        default=0.025,
        help="The p-value to use when testing for statistical significance",
    )

    opts = argp.parse_args()

    Outcome.__critical_p__ = opts.pvalue

    renderer = {"terminal": TerminalRenderer, "markdown": MarkdownRenderer}[
        opts.format
    ]()

    renderer.render_header("Austin Benchmarks")
    renderer.render_paragraph(
        f"Running Austin benchmarks with Python {'.'.join(str(_) for _ in sys.version_info[:3])}",
    )

    results: t.List[t.Tuple[str, t.List[Results]]] = []

    for variant, title, args in SCENARIOS:
        if opts.k is not None and not opts.k.match(title):
            continue

        table: t.List[Results] = []
        for version in VERSIONS[-2:] if opts.last else VERSIONS:
            print(f"> Running with Austin {version} ...    ", end="\r", file=sys.stderr)
            try:
                austin = download_release(version, Path("/tmp"), variant_name=variant)
            except RuntimeError:
                print(
                    f"WARNING: Could not download {variant} {version}", file=sys.stderr
                )
                continue

            stats = [
                _
                for _ in (get_stats(austin(*args).stdout) for _ in range(opts.n))
                if _ is not None
            ]
            table.append(
                (
                    version,
                    {
                        key: Outcome([s[key] for s in stats])
                        for key in list(stats[0].keys())
                    },
                )
            )

        results.append((title, table))

    summary = summarize(results)

    renderer.render_summary(summary)

    renderer.render_header("Benchmark Results", level=2)
    for title, table in results:
        renderer.render_scenario(title, table)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye!")
