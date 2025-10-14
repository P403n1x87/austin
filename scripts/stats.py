import typing as t
from collections import Counter
from io import BytesIO
from itertools import chain

import numpy as np
from austin.format.mojo import MojoStreamReader
from austin.events import AustinSample, AustinFrame
from scipy.stats import f

Stack = tuple[str, float]  # (stack frames, metric)


class AustinFlameGraph(dict):
    def __call__(self, x) -> float:
        return self.get(x, 0)

    def __add__(self, other: "AustinFlameGraph") -> "AustinFlameGraph":
        m = self.__class__(self)
        for k, v in other.items():
            n = m.setdefault(k, v.__class__()) + v
            if not n and k in m:
                del m[k]
                continue
            m[k] = n
        return m

    def __mul__(self, other: float) -> "AustinFlameGraph":
        m = self.__class__(self)
        for k, v in self.items():
            n = v * other
            if not n and k in m:
                del m[k]
                continue
            m[k] = n
        return m

    def __rmul__(self, other: float) -> "AustinFlameGraph":
        return self.__mul__(other)

    def __truediv__(self, other: float) -> "AustinFlameGraph":
        return self * (1 / other)

    def __rtruediv__(self, other: float) -> "AustinFlameGraph":
        return self.__truediv__(other)

    def __sub__(self, other: "AustinFlameGraph") -> "AustinFlameGraph":
        return self + (-other)

    def __neg__(self) -> "AustinFlameGraph":
        m = self.__class__(self)
        for k, v in m.items():
            m[k] = -v
        return m

    def supp(self) -> t.Set[str]:
        return set(self.keys())

    def to_list(self, domain: list) -> list:
        return [self(v) for v in domain]

    @classmethod
    def from_list(cls, stacks: t.List[Stack]) -> "AustinFlameGraph":
        return sum((cls({stack: metric}) for stack, metric in stacks), cls())

    @classmethod
    def from_mojo(cls, data: bytes) -> "AustinFlameGraph":
        fg = cls()

        def serialize(frame: AustinFrame) -> str:
            return ":".join(
                (
                    frame.filename,
                    frame.function,
                    str(frame.line),
                    str(frame.line_end),
                    str(frame.column),
                    str(frame.column_end),
                )
            )

        for e in MojoStreamReader(BytesIO(data)):
            if isinstance(e, AustinSample) and e.frames:
                fg += cls({";".join(serialize(f) for f in e.frames): e.metrics.time})

        return fg


def hotelling_two_sample_test(X, Y) -> float:
    nx, p = X.shape
    ny, q = Y.shape

    assert p == q, "X and Y must have the same dimensionality"

    dof = nx + ny - p - 1

    assert dof > 0, (
        f"X ({nx}x{p}) and Y ({ny}x{q}) must have at least p ({p}) + 1 samples"
    )

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
        c: t.Counter[str] = Counter()
        for _ in chain(x, y):
            c.update(_.supp())
        domain = sorted([k for k, v in c.items() if v >= threshold])

    X = np.array([f.to_list(domain) for f in x], dtype=np.int32)
    Y = np.array([f.to_list(domain) for f in y], dtype=np.int32)

    return hotelling_two_sample_test(X, Y)
