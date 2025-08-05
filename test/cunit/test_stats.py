import test.cunit.stats as stats
import time


def spinlock(duration: float) -> None:
    end = time.monotonic() + duration
    while time.monotonic() < end:
        pass


def test_stats_gettime():
    stats.stats_start()

    begin = stats.gettime()

    spinlock(1)

    delta = stats.gettime() - begin

    assert 0.95 <= delta / 1e6 <= 1.05


def test_stats_duration():
    stats.stats_start()

    spinlock(1)

    assert 0.95 <= stats.stats_duration() / 1e6 <= 1.05
