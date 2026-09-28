import os
import socket
import sys
from contextlib import contextmanager
from pathlib import Path
from subprocess import PIPE
from subprocess import CalledProcessError
from subprocess import Popen
from subprocess import check_output
from tempfile import TemporaryDirectory
from test.utils import allpythons
from test.utils import austin
from test.utils import has_frame
from test.utils import requires_sudo
from test.utils import run_python
from test.utils import threads
from threading import Thread
from time import monotonic
from time import sleep

import psutil
import pytest
from requests import get

pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="Not supported on Windows"
)

UWSGI = Path(__file__).parent / "uwsgi"


def _wait_for_port(
    host: str, port: int, timeout: float = 10.0, interval: float = 0.1
) -> None:
    """Poll a TCP port until something is listening on it.

    uWSGI's startup (Python interpreter init, WSGI app import, worker
    fork) can take longer than a fixed sleep on a loaded CI runner. A raw
    connect() succeeding is a much more direct "is uwsgi actually up"
    signal than guessing a fixed delay -- and, unlike an HTTP-level
    readiness probe, it doesn't have to wait through the test app's own
    sleep(2) on every request just to check whether the server is there.
    """
    deadline = monotonic() + timeout
    while True:
        try:
            with socket.create_connection((host, port), timeout=interval):
                return
        except OSError:
            if monotonic() >= deadline:
                raise TimeoutError(
                    f"uwsgi did not start listening on {host}:{port} within {timeout}s"
                )
            sleep(interval)


@contextmanager
def uwsgi(app="app.py", port=9090, args=[], env=None):
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
        env=env or os.environ,
    ) as uw:
        _wait_for_port("localhost", port)

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


@contextmanager
def venv(py, reqs):
    with TemporaryDirectory() as tmp_path:
        venv_path = Path(tmp_path) / ".venv"
        p = run_python(py, "-m", "venv", str(venv_path))
        p.wait(120)
        assert 0 == p.returncode, "Virtual environment was created successfully"

        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = str(venv_path / "lib")
        env["PATH"] = str(venv_path / "bin") + os.pathsep + env["PATH"]

        exc = None
        for attempt in range(3):
            try:
                check_output(
                    ["python", "-m", "pip", "install", "-r", reqs, "--use-pep517"],
                    env=env,
                )
                break
            except CalledProcessError as e:
                exc = e
        else:
            raise RuntimeError(
                f"Failed to install requirements after {attempt + 1} attempts: "
                f"{exc.output.decode()}"
            )

        yield env


@requires_sudo
@allpythons()
def test_uwsgi(py):
    responses = []
    request_thread = Thread(
        target=lambda: responses.append(get("http://localhost:9090"))
    )

    with venv(py, reqs=Path(__file__).parent / "requirements-uwsgi.txt") as env:
        with uwsgi(env=env) as uw:
            request_thread.start()

            # -x 3 gives a little slack over the app's own sleep(2): the
            # request still has to be routed to a worker after uwsgi itself
            # is confirmed listening, and that hand-off isn't instantaneous.
            result = austin(
                "-x", "3", "-i", "100ms", "-Cp", str(uw.pid), expect_fail=True
            )
            assert has_frame(result.samples, "app.py", "application", 5)

        request_thread.join()
        # A failed request raises inside the thread and is otherwise
        # swallowed -- surface it as a clear failure instead of leaving the
        # has_frame assertion above to fail confusingly on its own.
        assert responses and responses[0].status_code == 200, responses


@requires_sudo
@allpythons()
def test_uwsgi_multiprocess(py):
    responses = []
    request_thread = Thread(
        target=lambda: responses.append(get("http://localhost:9091"))
    )

    with venv(py, reqs=Path(__file__).parent / "requirements-uwsgi.txt") as env:
        with uwsgi(
            port=9091, args=["--processes", "2", "--threads", "2"], env=env
        ) as uw:
            request_thread.start()

            # -x 3 gives a little slack over the app's own sleep(2): the
            # request still has to be routed to a worker after uwsgi itself
            # is confirmed listening, and that hand-off isn't instantaneous.
            result = austin(
                "-x", "3", "-i", "100ms", "-Cp", str(uw.pid), expect_fail=True
            )
            assert has_frame(result.samples, "app.py", "application", 5)

            ts = threads(result.samples)
            assert len(ts) >= 4, ts
            assert len({p for p, _, _ in ts}) >= 2, ts

        request_thread.join()
        # A failed request raises inside the thread and is otherwise
        # swallowed -- surface it as a clear failure instead of leaving the
        # has_frame assertion above to fail confusingly on its own.
        assert responses and responses[0].status_code == 200, responses
