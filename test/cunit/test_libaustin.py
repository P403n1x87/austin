import os
import sys
import test.cunit.libaustin as la
from test.utils import requires_sudo

import pytest


@pytest.fixture
def handle():
    la.austin_up()

    pid = os.getpid()
    handle = la.austin_attach(pid)

    assert handle

    yield handle

    la.austin_detach(handle)

    la.austin_down()


def test_libaustin_up_down():
    for _ in range(10):
        la.austin_up()
        la.austin_down()


@requires_sudo
def test_libaustin_sample(handle):
    last_frame = None
    seen_pid = None

    @la.austin_callback
    def cb(pid, tid):
        nonlocal last_frame, seen_pid

        seen_pid = pid
        while True:
            frame = la.austin_pop_frame()
            if not frame:
                return

            last_frame = frame.contents

    la.austin_sample(handle, cb)

    frame = sys._getframe()

    assert seen_pid == os.getpid()

    assert last_frame.filename.decode() == frame.f_code.co_filename
    assert last_frame.scope.decode() == frame.f_code.co_name


@requires_sudo
def test_libaustin_read_frame(handle):
    frame = sys._getframe()

    austin_frame = la.austin_read_frame(handle, id(frame)).contents

    assert austin_frame.filename.decode() == frame.f_code.co_filename
    assert austin_frame.scope.decode() == frame.f_code.co_name
