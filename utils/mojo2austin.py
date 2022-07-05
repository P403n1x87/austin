import sys
from dataclasses import dataclass
from enum import Enum


class MojoEvents:
    RESERVED = 0
    METADATA = 1
    STACK = 2
    FRAME = 3
    FRAME_INVALID = 4
    FRAME_REF = 5
    FRAME_KERNEL = 6
    GC = 7
    IDLE = 8
    METRIC_TIME = 9
    METRIC_MEMORY = 10


def handles(e):
    def _(f):
        f.__event__ = e
        return f

    return _


class MojoMetricType(Enum):
    TIME = 0
    MEMORY = 1


class MojoEvent:
    pass

    def __str__(self):
        return ""


class MojoIdle(MojoEvent):
    pass


@dataclass
class MojoMetadata(MojoEvent):
    key: str
    value: str

    def __str__(self):
        return f"# {self.key}: {self.value}\n"


@dataclass
class MojoStack(MojoEvent):
    pid: bytes
    tid: str

    def __str__(self):
        return f"P{self.pid};T{int(self.tid, 16)}"


@dataclass
class MojoFrame(MojoEvent):
    key: bytes
    filename: str
    scope: str
    line: int


@dataclass
class MojoKernelFrame(MojoEvent):
    scope: str

    def __str__(self):
        return f";kernel:{self.scope}:0"


@dataclass
class MojoSpecialFrame(MojoEvent):
    label: str

    def __str__(self):
        return f";:{self.label}:"


@dataclass
class MojoFrameReference(MojoEvent):
    frame: MojoFrame

    def __str__(self):
        return f";{self.frame.filename}:{self.frame.scope}:{self.frame.line}"


@dataclass
class MojoMetric(MojoEvent):
    metric_type: MojoMetricType
    value: int

    def __str__(self):
        return f" {self.value}\n"


@dataclass
class MojoFullMetrics(MojoEvent):
    metrics: list[MojoMetric]

    def __str__(self):
        if len(self.metrics) == 3:
            time, idle, memory = self.metrics
            assert idle == 1, self.metrics
        else:
            time, memory = self.metrics
            idle = 0

        return f" {time.value},{idle},{memory.value}\n"


class MojoFile:
    __handlers__ = None

    def __init__(self, mojo):
        if self.__handlers__ is None:
            self.__class__.__handlers__ = {
                f.__event__: f
                for f in self.__class__.__dict__.values()
                if hasattr(f, "__event__")
            }

        self.mojo = mojo
        self._metrics = []
        self._full_mode = False
        self._frame_map = {}
        self._offset = 0
        self._last_read = 0

        assert self.read(3) == b"MOJ"

        self.mojo_version = self.read_int()

    def read(self, n):
        self._offset += self._last_read
        self._last_read = n
        return self.mojo.read(n)

    def read_int(self):
        n = 0
        s = 6
        (b,) = self.read(1)
        n |= b & 0x3F
        sign = b & 0x40
        while b & 0x80:
            (b,) = self.read(1)
            n |= (b & 0x7F) << s
            s += 7
        return -n if sign else n

    def read_until(self, until=b"\0"):
        buffer = bytearray()
        while True:
            b = self.read(1)
            if not b or b == until:
                return bytes(buffer)
            buffer += b

    def read_string(self):
        return self.read_until().decode()

    def _emit_metrics(self):
        if self._metrics:
            yield MojoFullMetrics(
                self._metrics
            ) if self._full_mode else self._metrics.pop()
            self._metrics.clear()

    @handles(MojoEvents.METADATA)
    def parse_metadata(self):
        yield from self._emit_metrics()

        metadata = MojoMetadata(self.read_string(), self.read_string())
        if metadata.key == "mode" and metadata.value == "full":
            self._full_mode = True
        yield metadata

    @handles(MojoEvents.STACK)
    def parse_stack(self):
        yield from self._emit_metrics()

        yield MojoStack(self.read_int(), self.read_string())

    @handles(MojoEvents.FRAME)
    def parse_frame(self):
        frame_key = self.read_int()
        filename = self.read_string()
        scope = self.read_string()
        line = self.read_int()

        frame = MojoFrame(frame_key, filename, scope, line)

        self._frame_map[frame_key] = frame

        yield frame

    @handles(MojoEvents.FRAME_REF)
    def parse_frame_ref(self):
        yield MojoFrameReference(self._frame_map[self.read_int()])

    @handles(MojoEvents.FRAME_KERNEL)
    def parse_kernel_frame(self):
        yield MojoKernelFrame(self.read_string())

    def _parse_metric(self, metric_type):
        metric = MojoMetric(
            metric_type,
            self.read_int(),
        )

        if self._full_mode:
            self._metrics.append(metric)
            return MojoEvent()

        return metric

    @handles(MojoEvents.METRIC_TIME)
    def parse_time_metric(self):
        yield self._parse_metric(MojoMetricType.TIME)

    @handles(MojoEvents.METRIC_MEMORY)
    def parse_memory_metric(self):
        yield self._parse_metric(MojoMetricType.MEMORY)

    @handles(MojoEvents.FRAME_INVALID)
    def parse_invalid_frame(self):
        yield MojoSpecialFrame("INVALID")

    @handles(MojoEvents.IDLE)
    def parse_idle(self):
        self._metrics.append(1)
        yield MojoIdle()

    @handles(MojoEvents.GC)
    def parse_gc(self):
        yield MojoSpecialFrame("GC")

    def parse_event(self):
        try:
            (event,) = self.read(1)
        except ValueError:
            yield None
            return

        try:
            yield from self.__handlers__[event](self)
        except KeyError:
            raise ValueError(
                f"Unhandled event: {event} (offset: {self._offset}, last read: {self._last_read})"
            )

    def parse(self):
        while True:
            for e in self.parse_event():
                if e is None:
                    return
                yield e


def main():
    with open(sys.argv[1], "rb") as mojo:
        for event in MojoFile(mojo).parse():
            print(event, end="")


if __name__ == "__main__":
    main()
