import test.cunit.stats as stats
import time


def test_stats_gettime():
    begin = stats.gettime()
    time.sleep(1)
    delta = stats.gettime() - begin
    assert 0.95 <= delta / 1e6 <= 1.05


def test_stats_duration():
    stats.stats_start()
    time.sleep(1)
    dur = stats.stats_duration()
    assert 0.95 <= dur / 1e6 <= 1.05
