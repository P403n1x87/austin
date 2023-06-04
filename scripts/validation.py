# Run as python3 scripts/validation.py from the repository root directory.
# Ensure dependencies from requirements-val.txt are installed.

from argparse import ArgumentParser
from collections import Counter, namedtuple
from io import BytesIO
from itertools import chain
import os
from pathlib import Path
import re
import sys
import typing as t
import codecs

import common

import numpy as np
from scipy.stats import f

from austin.format.mojo import (
    MojoFile,
    MojoStack,
    MojoFrameReference,
    MojoMetric,
    MojoFrame,
)
from test.utils import python, target

Scenario = namedtuple("Scenario", ["title", "variant", "args"])

PYTHON = (
    python(os.getenv("AUSTIN_TESTS_PYTHON_VERSIONS"))
    if "AUSTIN_TESTS_PYTHON_VERSIONS" in os.environ
    else [sys.executable]
)

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
            "-si",
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
            "-Csi",
            "500",
            *PYTHON,
            target("target_mp.py"),
        ),
    ),
]


class AustinFlameGraph(dict):
    def __call__(self, x):
        return self.get(x, 0)

    def __add__(self, other):
        m = self.__class__(self)
        for k, v in other.items():
            n = m.setdefault(k, v.__class__()) + v
            if not n and k in m:
                del m[k]
                continue
            m[k] = n
        return m

    def __mul__(self, other):
        m = self.__class__(self)
        for k, v in self.items():
            n = v * other
            if not n and k in m:
                del m[k]
                continue
            m[k] = n
        return m

    def __rmul__(self, other):
        return self.__mul__(other)

    def __truediv__(self, other):
        return self * (1 / other)

    def __rtruediv__(self, other):
        return self.__div__(other)

    def __sub__(self, other):
        return self + (-other)

    def __neg__(self):
        m = self.__class__(self)
        for k, v in m.items():
            m[k] = -v
        return m

    def supp(self):
        return set(self.keys())

    def to_list(self, domain: list) -> list:
        return [self(v) for v in domain]

    @classmethod
    def from_list(cls, stacks: t.List[t.Tuple[str, int]]) -> "AustinFlameGraph":
        return sum((cls({stack: metric}) for stack, metric in stacks), cls())

    @classmethod
    def from_mojo(cls, data: bytes) -> "AustinFlameGraph":
        fg = cls()

        stack: t.List[str] = []
        metric = 0

        def serialize(frame: MojoFrame) -> str:
            return ":".join(
                (
                    frame.filename.string.value,
                    frame.scope.string.value,
                    str(frame.line),
                    str(frame.line_end),
                    str(frame.column),
                    str(frame.column_end),
                )
            )

        for e in MojoFile(BytesIO(data)).parse():
            if isinstance(e, MojoStack):
                if stack:
                    fg += cls({";".join(stack): metric})
                stack.clear()
                metric = 0
            elif isinstance(e, MojoFrameReference):
                stack.append(serialize(e.frame))
            elif isinstance(e, MojoMetric):
                metric = e.value

        return fg


def hotelling_two_sample_test(X, Y) -> float:
    nx, p = X.shape
    ny, q = Y.shape

    assert p == q, "X and Y must have the same dimensionality"

    dof = nx + ny - p - 1

    assert (
        dof > 0
    ), f"X ({nx}x{p}) and Y ({ny}x{q}) must have at least p ({p}) + 1 samples"

    g = dof / p / (nx + ny - 2) * (nx * ny) / (nx + ny)

    x_mean = np.mean(X, axis=0)
    y_mean = np.mean(Y, axis=0)
    delta = x_mean - y_mean

    x_cov = np.cov(X, rowvar=False)
    y_cov = np.cov(Y, rowvar=False)
    pooled_cov = ((nx - 1) * x_cov + (ny - 1) * y_cov) / (nx + ny - 2)

    # Compute the F statistic from the Hotelling T^2 statistic
    statistic = g * delta.transpose() @ np.linalg.inv(pooled_cov) @ delta
    f_pdf = f(p, dof)

    return 1 - f_pdf.cdf(statistic)


def compare(
    x: t.List[AustinFlameGraph],
    y: t.List[AustinFlameGraph],
    threshold: t.Optional[float] = None,
) -> float:
    domain = list(set().union(*(_.supp() for _ in chain(x, y))))

    if threshold is not None:
        c = Counter()
        for _ in chain(x, y):
            c.update(_.supp())
        domain = sorted([k for k, v in c.items() if v >= threshold])

    X = np.array([f.to_list(domain) for f in x], dtype=np.int32)
    Y = np.array([f.to_list(domain) for f in y], dtype=np.int32)

    return hotelling_two_sample_test(X, Y)


def validate(args, variant: str = "austin", runs: int = 10) -> float:
    austin_latest = common.download_latest(dest=Path("/tmp"), variant_name=variant)
    austin_dev = common.get_dev(variant_name=variant)

    return compare(
        *(
            [
                AustinFlameGraph.from_mojo(
                    austin(
                        *args,
                        mojo=True,
                        convert=False,
                    ).stdout
                )
                for _ in range(runs)
            ]
            for austin in (austin_latest, austin_dev)
        ),
        threshold=runs,  # Keep only the stacks that are present in all runs
    )


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

    opts = argp.parse_args()

    if opts.ignore_errors:
        codecs.register_error("strict", codecs.ignore_errors)

    print("# Austin Data Validation\n")

    failures: t.List[Scenario] = []
    for scenario in SCENARIOS:
        print(
            f"Validating {scenario.title} ...                                ",
            end="\r",
            flush=True,
            file=sys.stderr,
        )
        if (p := validate(scenario.args, scenario.variant, runs=opts.n)) < opts.p_value:
            failures.append((scenario, p))

    if failures:
        print("💥 The following scenarios failed to validate:\n")

        for scenario, p in failures:
            print(f"- {scenario.title} [{p:.2%}]")

        exit(1)

    print(f"✨ 🍰 ✨ All {len(SCENARIOS)} scenarios validated successfully!")
