# Run as python3 scripts/benchmark.py from the repository root directory.
# Ensure dependencies from requirements-bm.txt are installed.

import abc
import enum
import json
import platform
import re
import shutil
from subprocess import TimeoutExpired
import sys
import typing as t
from argparse import ArgumentParser
from dataclasses import dataclass
from math import floor, log
from pathlib import Path
from textwrap import wrap

from common import download_release
from scipy.stats import ttest_ind

from test.utils import target

VERSIONS = ("base", "dev")


def _find_free_threaded_python() -> t.Optional[str]:
    """Return the path to a free-threaded Python interpreter, or None."""
    for candidate in ("python3.13t", "python3.14t"):
        path = shutil.which(candidate)
        if path is not None:
            return path
    return None


FT_PYTHON = _find_free_threaded_python()


# ---- Metrics ---------------------------------------------------------------
# Each metric is a named, directional computation over the mojo metadata dict.
# Metric *sets* (Metrics subclasses) group related metrics together so that
# different kinds of scenarios can report different figures (e.g. native-mode
# scenarios include effective rate / observer overhead on top of the defaults).


class Correlation(enum.IntEnum):
    """Sign of the relationship between a metric and "better performance"."""

    POSITIVE = +1  # higher is better
    NEGATIVE = -1  # lower is better

    @classmethod
    def from_sign(cls, s: str) -> "Correlation":
        return cls.POSITIVE if s.strip().startswith("+") else cls.NEGATIVE


@dataclass(frozen=True)
class Metric:
    name: str
    unit: str
    correlation: Correlation
    compute: t.Callable[[t.Dict[str, str]], float]


# ---- Metric functions ------------------------------------------------------
# Each metric is a top-level function taking the mojo metadata dict.  The
# docstring carries the metadata consumed by ``MetricsMeta``:
#
#   unit:        <human-readable unit>
#   correlation: + (higher is better) | - (lower is better)
#
# The metric's display name is derived from the function name
# (``sample_rate`` -> ``"Sample Rate"``).


def sample_rate(meta):
    """
    unit: samples/sec
    correlation: +
    """
    return int(meta["count"]) / (float(meta["duration"]) / 1e6)


def effective_rate(meta):
    """
    unit: samples/sec
    correlation: +
    """
    # Samples per second of *target run time*, excluding time the target spent
    # suspended for sampling.  Makes implementations comparable independent of
    # the observer overhead.
    _, tick_cnt = (int(x) for x in meta["saturation"].split("/"))
    avg_sampling_us = int(meta["sampling"].split(",")[1])
    dur_us = float(meta["duration"])
    target_us = max(1.0, dur_us - tick_cnt * avg_sampling_us)
    return int(meta["count"]) / (target_us / 1e6)


def overhead(meta):
    """
    unit: %
    correlation: -
    """
    _, tick_cnt = (int(x) for x in meta["saturation"].split("/"))
    avg_sampling_us = int(meta["sampling"].split(",")[1])
    return tick_cnt * avg_sampling_us / float(meta["duration"]) * 100.0


def saturation(meta):
    """
    unit: ratio
    correlation: -
    """
    return eval(meta["saturation"])


def error_rate(meta):
    """
    unit: ratio
    correlation: -
    """
    return eval(meta["errors"])


def sampling_speed(meta):
    """
    unit: us
    correlation: -
    """
    return int(meta["sampling"].split(",")[1])


# ---- Metrics metaclass -----------------------------------------------------

_DOC_FIELD_RE = re.compile(r"^\s*(\w+)\s*:\s*(.+?)\s*$")


def _metric_from_fn(fn: t.Callable) -> Metric:
    """Build a ``Metric`` by parsing the function's name and docstring."""
    fields = {}
    for line in (fn.__doc__ or "").splitlines():
        match = _DOC_FIELD_RE.match(line)
        if match:
            fields[match.group(1).lower()] = match.group(2)

    try:
        correlation = Correlation.from_sign(fields["correlation"])
        unit = fields["unit"]
    except KeyError as exc:
        raise ValueError(
            f"Metric function {fn.__name__!r} is missing '{exc.args[0]}' in its docstring"
        ) from exc

    name = fn.__name__.replace("_", " ").title()
    return Metric(name=name, unit=unit, correlation=correlation, compute=fn)


METRICS_CLASSES: t.Dict[str, t.Type["Metrics"]] = {}


class MetricsMeta(type):
    """Metaclass for metric sets.  Use ``MetricsMeta.build(name, *fns)`` to
    assemble a concrete ``Metrics`` subclass from a list of docstring-annotated
    metric functions."""

    @classmethod
    def build(mcs, name: str, *functions: t.Callable) -> t.Type["Metrics"]:
        cls = t.cast(
            t.Type[Metrics],
            mcs(name, (Metrics,), {"ALL": [_metric_from_fn(fn) for fn in functions]}),
        )
        METRICS_CLASSES[name] = cls
        return cls


class Metrics(metaclass=MetricsMeta):
    """Base class for metric sets.  Build concrete subclasses via
    ``MetricsMeta.build(name, fn1, fn2, ...)``."""

    ALL: t.ClassVar[t.List[Metric]] = []

    @classmethod
    def compute(cls, meta: t.Dict[str, str]) -> t.Optional[t.Dict[str, float]]:
        try:
            return {m.name: m.compute(meta) for m in cls.ALL}
        except Exception:
            return None

    @classmethod
    def correlations(cls) -> t.List[t.Tuple[str, Correlation]]:
        return [(m.name, m.correlation) for m in cls.ALL]


NormalMetrics = MetricsMeta.build(
    "NormalMetrics", sample_rate, saturation, error_rate, sampling_speed
)

NativeMetrics = MetricsMeta.build(
    "NativeMetrics",
    sample_rate,
    effective_rate,
    overhead,
    error_rate,
    sampling_speed,
)


# ---- Scenarios -------------------------------------------------------------


@dataclass(frozen=True)
class Scenario:
    group: str
    title: str
    args: t.List[str]
    variant: str = "austin"
    metrics: t.Type[Metrics] = NormalMetrics

    @property
    def group_slug(self) -> str:
        return self.group.lower().replace(" ", "-")


SCENARIOS: t.List[Scenario] = [
    *[
        Scenario(
            group="Wall time",
            title=f"Wall time [sampling interval: {i}]",
            args=["-i", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        Scenario(
            group="CPU time",
            title=f"CPU time [sampling interval: {i}]",
            args=["-ci", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        Scenario(
            group="RSA keygen",
            title=f"RSA keygen [sampling interval: {i}]",
            args=["-ci", str(i), sys.executable, "-m", "test.bm.rsa_key_generator"],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        Scenario(
            group="Full metrics",
            title=f"Full metrics [sampling interval: {i}]",
            args=["-fi", str(i), sys.executable, target("target34.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        Scenario(
            group="Multiprocess wall time",
            title=f"Multiprocess wall time [sampling interval: {i}]",
            args=["-Cfi", str(i), sys.executable, target("target_mp.py"), "8"],
        )
        for i in (1, 10, 100, 1000)
    ],
    *[
        Scenario(
            group="Native wall time",
            title=f"Native wall time [sampling interval: {i}]",
            args=["-ni", str(i), sys.executable, target("target34.py")],
            metrics=NativeMetrics,
        )
        for i in (100, 1000, 10000)
    ],
    *[
        Scenario(
            group="Asyncio wall time",
            title=f"Asyncio wall time [sampling interval: {i}]",
            # Three threads, each driving its own event loop with several
            # tasks -- the per-thread asyncio task scan (3.14+ only) runs
            # once per thread per sample, so this is where its overhead, if
            # any, would actually show up.
            args=["-i", str(i), sys.executable, target("target_asyncio_multiloop.py")],
        )
        for i in (1, 10, 100, 1000)
    ],
    *(
        [
            *[
                Scenario(
                    group="Free-threaded wall time",
                    title=f"Free-threaded wall time [sampling interval: {i}]",
                    args=["-i", str(i), FT_PYTHON, target("target34.py")],
                )
                for i in (1, 10, 100, 1000)
            ],
            *[
                Scenario(
                    group="Free-threaded CPU time",
                    title=f"Free-threaded CPU time [sampling interval: {i}]",
                    args=["-ci", str(i), FT_PYTHON, target("target34.py")],
                )
                for i in (1, 10, 100, 1000)
            ],
            *[
                Scenario(
                    group="Free-threaded full metrics",
                    title=f"Free-threaded full metrics [sampling interval: {i}]",
                    args=["-fi", str(i), FT_PYTHON, target("target34.py")],
                )
                for i in (1, 10, 100, 1000)
            ],
            *[
                Scenario(
                    group="Free-threaded native wall time",
                    title=f"Free-threaded native wall time [sampling interval: {i}]",
                    args=["-ni", str(i), FT_PYTHON, target("target34.py")],
                    metrics=NativeMetrics,
                )
                for i in (100, 1000, 10000)
            ],
        ]
        if FT_PYTHON
        else []
    ),
]

# Ordered unique groups, derived from SCENARIOS (preserves definition order).
SCENARIO_GROUPS: t.List[str] = list(dict.fromkeys(s.group for s in SCENARIOS))


# Map scenario title -> metrics class (used during merge/summarise when only
# the title survives in the intermediate JSON).
_METRICS_BY_TITLE: t.Dict[str, t.Type[Metrics]] = {
    s.title: s.metrics for s in SCENARIOS
}


def _metrics_for(title: str, fallback_name: t.Optional[str] = None) -> t.Type[Metrics]:
    if title in _METRICS_BY_TITLE:
        return _METRICS_BY_TITLE[title]
    if fallback_name and fallback_name in METRICS_CLASSES:
        return METRICS_CLASSES[fallback_name]
    return NormalMetrics


def _platform_info() -> t.Dict[str, str]:
    return {
        "platform": sys.platform,
        "arch": platform.machine(),
        "python": ".".join(str(x) for x in sys.version_info[:3]),
    }


def _platform_label(info: t.Dict[str, str]) -> str:
    p = info.get("platform", "unknown")
    a = info.get("arch", "unknown")
    return f"{p} {a}" if a else p


class Outcome:
    __critical_p__ = 0.025

    def __init__(self, data: list[float]) -> None:
        self.data = data
        self.mean = sum(data) / len(data)
        self.stdev = (
            (sum(((v - self.mean) ** 2 for v in data)) / (len(data) - 1)) ** 0.5
            if len(data) > 1
            else 0.0
        )

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
    def render_header(self, title: str, level: int = 1) -> None: ...

    @abc.abstractmethod
    def render_paragraph(self, text: str) -> None: ...

    @abc.abstractmethod
    def render_table(self, table) -> None: ...

    @abc.abstractmethod
    def render_scenario(
        self, title, results: t.List[t.Tuple[str, t.List[Results]]]
    ) -> None: ...

    @abc.abstractmethod
    def render_summary(
        self,
        summary: t.List[
            t.Tuple[str, t.Type["Metrics"], t.List[t.Tuple[str, bool, int]]]
        ],
        skipped: t.List[str],
        level: int = 2,
    ) -> None: ...

    def open_group(self, label: str, level: int) -> None:
        self.render_header(label, level=level)

    def close_group(self) -> None:
        pass


class TerminalRenderer(Renderer):
    def render_scenario(
        self, title, table: t.List[t.Tuple[str, t.List[Results]]]
    ) -> None:
        self.render_header(title, level=2)
        self.render_table(table)
        print()

    def render_summary(self, summary, skipped, level=2):
        if not summary:
            self.render_paragraph(
                "No significant difference in performance between versions."
            )
        else:
            self.render_paragraph(
                "The following scenarios show a statistically significant difference "
                "in performance between the two versions."
            )

            # Partition by metrics class — metric columns differ between
            # Normal and Native scenarios, so we render a table per class.
            by_metrics: t.Dict[t.Type[Metrics], t.List[t.Tuple[str, list]]] = {}
            for title, metrics_class, tests in summary:
                by_metrics.setdefault(metrics_class, []).append((title, tests))

            multi = len(by_metrics) > 1
            for metrics_class, rows in by_metrics.items():
                if multi:
                    self.render_header(metrics_class.__name__, level=level + 1)
                self.render_table(
                    [
                        (
                            title,
                            {
                                m: {1: self.BETTER, -1: self.WORSE}[s]
                                if c
                                else self.SAME
                                for m, c, s in tests
                            },
                        )
                        for title, tests in rows
                    ]
                )

        if skipped:
            self.render_paragraph(
                "The following scenarios were skipped because the base version "
                "produced no data (e.g. unsupported flag or new Python version):"
            )
            for s in skipped:
                print(f"- {s}")
            print()

    def render_table(self, table: t.List[t.Tuple[str, t.List[Results]]]) -> None:
        _, row = table[0]
        cols = list(row.keys())
        max_vh = max(len(e[0]) for e in table)

        col_widths = [max(max(len(r[col]), len(col)) for _, r in table) for col in cols]
        div_len = sum(col_widths) + (len(cols) + 1) * 2 + max_vh

        print("=" * div_len)
        print(
            (" " * (max_vh + 2))
            + "".join(f"{col:^{cw + 2}}" for col, cw in zip(cols, col_widths))
        )
        print("-" * div_len)

        for v, row in table:
            print(f"{v:^{max_vh + 2}}", end="")
            for col, cw in zip(cols, col_widths):
                print(f"{str(row[col]):^{cw + 2}}", end="")
            print()

        print("=" * div_len)
        print()

    def render_header(self, title: str, level: int = 1) -> None:
        print(title)
        print({1: "=", 2: "-", 3: "~"}.get(level, "-") * len(title))
        print()

    def render_paragraph(self, text: str) -> None:
        for _ in wrap(text):
            print(_)
        print()


class MarkdownRenderer(TerminalRenderer):
    BETTER = ":green_circle:"
    WORSE = ":red_circle:"
    SAME = ":yellow_circle:"

    def render_header(self, title: str, level: int = 1) -> None:
        print(f"{'#' * level} {title}")
        print()

    def render_paragraph(self, text: str) -> None:
        print(text)
        print()

    def render_table(self, table: t.List[t.Tuple[str, t.List[Results]]]) -> None:
        _, row = table[0]
        cols = list(row.keys())
        col_widths = [max(max(len(r[col]), len(col)) for _, r in table) for col in cols]

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
        print()

    def render_scenario(
        self, title, table: t.List[t.Tuple[str, t.List[Results]]]
    ) -> None:
        print("<details>")
        print(f"<summary><strong>{title}</strong></summary>")
        print()
        super().render_scenario(title, table)
        print("</details>")
        print()

    def open_group(self, label: str, level: int) -> None:
        del level  # details expander is self-contained; level is ignored
        print("<details>")
        print(f"<summary><strong>{label}</strong></summary>")
        print()

    def close_group(self) -> None:
        print("</details>")
        print()


# ---- Report data structures ------------------------------------------------


@dataclass
class ScenarioEntry:
    title: str
    metrics_class: t.Type[Metrics]
    table: t.List[Results]  # [(version, {metric_name: Outcome})]


@dataclass
class PlatformReport:
    info: t.Dict[str, str]  # platform, arch, python
    scenarios: t.List[ScenarioEntry]
    skipped: t.List[str]

    @property
    def label(self) -> str:
        return _platform_label(self.info)


def summarize(
    entries: t.List[ScenarioEntry],
) -> t.List[t.Tuple[str, t.Type[Metrics], t.List[t.Tuple[str, bool, int]]]]:
    """Build significance summary, one row per scenario that has any
    statistically-significant metric change between base and dev."""
    summary = []
    for entry in entries:
        (_, a), (_, b) = entry.table[-2:]
        tests = []
        for m in entry.metrics_class.ALL:
            if m.name not in a or m.name not in b:
                continue
            delta = b[m.name].mean - a[m.name].mean
            sign = int(delta * m.correlation / (abs(delta) or 1))
            tests.append((m.name, a[m.name] == b[m.name], sign))
        if any(c for _, c, _ in tests):
            summary.append((entry.title, entry.metrics_class, tests))
    return summary


# ---- JSON (de)serialisation ------------------------------------------------


def results_to_json(report: PlatformReport) -> str:
    return json.dumps(
        {
            **report.info,
            "results": [
                {
                    "title": e.title,
                    "metrics_class": e.metrics_class.__name__,
                    "table": [
                        {
                            "version": v,
                            "metrics": {k: o.data for k, o in m.items()},
                        }
                        for v, m in e.table
                    ],
                }
                for e in report.scenarios
            ],
            "skipped": report.skipped,
        },
        indent=2,
    )


def results_from_json(raw: str) -> PlatformReport:
    doc = json.loads(raw)
    # Legacy format: top-level is a list of scenario entries without a
    # metrics_class field.  Treat as unknown platform / NormalMetrics.
    if isinstance(doc, list):
        doc = {"results": doc}

    entries = []
    for entry in doc.get("results", []):
        metrics_class = _metrics_for(entry["title"], entry.get("metrics_class"))
        table = []
        for row in entry["table"]:
            metrics = {k: Outcome(v) for k, v in row["metrics"].items()}
            table.append((row["version"], metrics))
        entries.append(ScenarioEntry(entry["title"], metrics_class, table))

    return PlatformReport(
        info={k: doc[k] for k in ("platform", "arch", "python") if k in doc},
        scenarios=entries,
        skipped=list(doc.get("skipped", [])),
    )


def merge_results(parts: t.List[PlatformReport]) -> t.List[PlatformReport]:
    """Group partial reports by (platform, arch); within each group, dedupe
    scenarios by title and sort them in SCENARIOS order."""
    order = {s.title: i for i, s in enumerate(SCENARIOS)}
    grouped: t.Dict[t.Tuple[str, str], PlatformReport] = {}
    for part in parts:
        key = (part.info.get("platform", ""), part.info.get("arch", ""))
        if key not in grouped:
            grouped[key] = PlatformReport(
                info=dict(part.info), scenarios=[], skipped=[]
            )
        existing = grouped[key]
        seen = {e.title for e in existing.scenarios}
        for e in part.scenarios:
            if e.title not in seen:
                existing.scenarios.append(e)
                seen.add(e.title)
        existing.skipped.extend(s for s in part.skipped if s not in existing.skipped)

    for report in grouped.values():
        report.scenarios.sort(key=lambda e: order.get(e.title, len(order)))

    return list(grouped.values())


# ---- Benchmark runner ------------------------------------------------------


def benchmark(opts: ArgumentParser) -> None:
    Outcome.__critical_p__ = opts.pvalue

    entries: t.List[ScenarioEntry] = []
    skipped: t.List[str] = []

    for scenario in SCENARIOS:
        if opts.k is not None and not opts.k.search(scenario.title):
            continue

        print(f"Running scenario {scenario.title} ...", file=sys.stderr)

        table: t.List[Results] = []
        for version in VERSIONS:
            print(f"> Running with Austin {version} ...    ", end="\r", file=sys.stderr)
            try:
                austin = download_release(
                    version, Path("/tmp"), variant_name=scenario.variant
                )
            except RuntimeError:
                print(
                    f"WARNING: Could not download {scenario.variant} {version}",
                    file=sys.stderr,
                )
                continue

            runs = []
            for _ in range(opts.n):
                try:
                    runs.append(austin(*scenario.args))
                except (RuntimeError, TimeoutExpired):
                    break  # binary doesn't support these args or timed out
            stats = [
                s
                for s in (scenario.metrics.compute(r.metadata) for r in runs)
                if s is not None
            ]
            if not stats:
                print(
                    f"WARNING: No valid stats for {scenario.variant} {version} "
                    f"with args {scenario.args}",
                    file=sys.stderr,
                )
                continue
            table.append(
                (
                    version,
                    {
                        key: Outcome([s[key] for s in stats])
                        for key in list(stats[0].keys())
                    },
                )
            )

        if len(table) < 2:
            print(
                f"WARNING: Skipping scenario {scenario.title!r} — "
                "insufficient data (base may not support this scenario)",
                file=sys.stderr,
            )
            skipped.append(scenario.title)
            continue

        entries.append(ScenarioEntry(scenario.title, scenario.metrics, table))

    report = PlatformReport(info=_platform_info(), scenarios=entries, skipped=skipped)

    if opts.format == "json":
        print(results_to_json(report))
        return

    render([report], opts)


def render(reports: t.List[PlatformReport], opts: ArgumentParser) -> None:
    renderer = {"terminal": TerminalRenderer, "markdown": MarkdownRenderer}[
        opts.format
    ]()

    renderer.render_header("Austin Benchmarks")
    renderer.render_paragraph(
        f"Running Austin benchmarks with Python {'.'.join(str(_) for _ in sys.version_info[:3])}",
    )

    multi_platform = len(reports) > 1
    platform_level = 3 if multi_platform else 2

    # --- Benchmark Summary (grouped per platform) -------------------------
    renderer.render_header("Benchmark Summary", level=2)
    renderer.render_paragraph(
        f"Comparison of **{VERSIONS[-1]}** against **{VERSIONS[-2]}**."
    )
    for report in reports:
        if multi_platform:
            renderer.render_header(report.label, level=platform_level)
        summary = summarize(report.scenarios)
        renderer.render_summary(summary, report.skipped, level=platform_level)

    # --- Benchmark Results (grouped per platform, collapsible in markdown) -
    renderer.render_header("Benchmark Results", level=2)
    for report in reports:
        if multi_platform:
            renderer.open_group(report.label, level=platform_level)

        # Partition entries by metrics class, preserving scenario order.
        by_metrics: t.Dict[t.Type[Metrics], t.List[ScenarioEntry]] = {}
        for e in report.scenarios:
            by_metrics.setdefault(e.metrics_class, []).append(e)

        multi_metrics = len(by_metrics) > 1
        for metrics_class, group in by_metrics.items():
            if multi_metrics:
                renderer.render_header(
                    metrics_class.__name__, level=platform_level + 1
                )
            for entry in group:
                renderer.render_scenario(entry.title, entry.table)

        if multi_platform:
            renderer.close_group()


def main():
    argp = ArgumentParser()

    argp.add_argument(
        "-k",
        type=re.compile,
        help="Run benchmark scenarios matching the given regular expression",
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
        choices=["terminal", "markdown", "json"],
        default="terminal",
        help="The output format",
    )

    argp.add_argument(
        "-p",
        "--pvalue",
        type=float,
        default=0.025,
        help="The p-value to use when testing for statistical significance",
    )

    argp.add_argument(
        "--merge",
        nargs="+",
        metavar="FILE",
        help="Merge JSON result files produced by --format json and render a report",
    )

    argp.add_argument(
        "--list-groups",
        action="store_true",
        help="Print the benchmark groups as a GitHub Actions matrix JSON and exit",
    )

    opts = argp.parse_args()

    if opts.list_groups:
        matrix = [
            {"name": g.lower().replace(" ", "-"), "filter": g} for g in SCENARIO_GROUPS
        ]
        print(json.dumps({"include": matrix}))
        return

    if opts.merge:
        parts = [results_from_json(Path(f).read_text()) for f in opts.merge]
        reports = merge_results(parts)
        # Any scenario title not present in any report is considered skipped.
        present = {e.title for r in reports for e in r.scenarios}
        missing = [s.title for s in SCENARIOS if s.title not in present]
        for r in reports:
            for m in missing:
                if m not in r.skipped:
                    r.skipped.append(m)
        if opts.format == "json":
            # When merging, preserve each platform as its own document.
            print(
                json.dumps([json.loads(results_to_json(r)) for r in reports], indent=2)
            )
        else:
            render(reports, opts)
        return

    benchmark(opts)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nBye!")
