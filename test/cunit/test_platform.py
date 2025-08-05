import sys
from test.cunit.platform import pid_max

import pytest


@pytest.mark.skipif(sys.platform != "linux", reason="Only applicable on Linux")
def test_pid_max():
    with open("/proc/sys/kernel/pid_max", "rb") as f:
        assert pid_max() == int(f.read()) > (1 << 15)
