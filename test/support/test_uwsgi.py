import sys
from contextlib import contextmanager
from pathlib import Path
from subprocess import PIPE
from subprocess import Popen
from test.utils import austin
from test.utils import compress
from test.utils import has_pattern
from test.utils import requires_sudo
from test.utils import threads
from threading import Thread
from time import sleep

import psutil
import pytest
from requests import get


pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="Not supported on Windows"
)

UWSGI = Path(__file__).parent / "uwsgi"


@contextmanager
def uwsgi(app="app.py", port=9090, args=[]):
    with Popen(
        [
            "uwsgi",
            "--http",
            f":{port}",
            "--wsgi-file",
            UWSGI / app,
            *args,
        ],
        stdout=PIPE,
        stderr=PIPE,
    ) as uw:
        sleep(0.5)

        assert uw.poll() is None, uw.stderr.read().decode()

        pid = uw.pid
        assert pid is not None, uw.stderr.read().decode()

        try:
            yield uw
        finally:
            uw.kill()
            # Sledgehammer
            for proc in psutil.process_iter():
                if "uwsgi" in proc.name():
                    proc.terminate()
                    proc.wait()


@requires_sudo
def test_uwsgi():
    request_thread = Thread(target=get, args=("http://localhost:9090",))

    with uwsgi() as uw:
        request_thread.start()

        result = austin("-x", "2", "-Cp", str(uw.pid))
        assert has_pattern(result.stdout, "app.py:application:5"), compress(
            result.stdout
        )

    request_thread.join()


@requires_sudo
def test_uwsgi_multiprocess():
    request_thread = Thread(target=get, args=("http://localhost:9091",))

    with uwsgi(port=9091, args=["--processes", "2", "--threads", "2"]) as uw:
        request_thread.start()

        result = austin("-x", "2", "-Cp", str(uw.pid))
        assert has_pattern(result.stdout, "app.py:application:5"), compress(
            result.stdout
        )

        ts = threads(result.stdout)
        assert len(ts) >= 4, ts
        assert len({p for p, _ in ts}) >= 2, ts

    request_thread.join()
