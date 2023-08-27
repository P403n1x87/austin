from test.cunit.error import cglobal
from test.cunit.error import error_get_msg
from test.cunit.error import is_fatal

import pytest


@pytest.mark.parametrize(
    "code, msg",
    [
        (0, "No error"),
        (1, "Cannot open memory maps file"),
        (8, "Failed to retrieve PyCodeObject"),
        (16, "Failed to create frame object"),
        (24, "Failed to create thread object"),
        (32, "Failed to initialise process"),
    ],
)
def test_error_get_msg(code, msg):
    assert error_get_msg(code) == msg.encode()


def test_error_unknown():
    assert error_get_msg(10000) == b"<Unknown error>"


@pytest.mark.parametrize(
    "code, fatal",
    [
        (0, False),
        (1, True),
        (8, False),
        (32, False),
        (1000, False),
    ],
)
def test_error_is_fatal(code, fatal):
    assert is_fatal(code) == fatal


def test_error_austin_errno_global():
    assert cglobal("austin_errno", "int") == 0
