import sys
from test.cunit.error import cglobal
from test.cunit.error import error_get_msg
from test.cunit.error import is_fatal
from test.error import AustinError

import pytest


def test_error_get_msg():
    for code in AustinError:
        assert error_get_msg(code).decode() == AustinError.message(code)


def test_error_unknown():
    assert error_get_msg(10000) == b"<Unknown error>"


def test_error_is_fatal():
    for code in AustinError:
        assert is_fatal(code) == AustinError.is_fatal(code)


@pytest.mark.skipif(sys.platform != "linux", reason="Only applicable on Linux")
def test_error_austin_errno_global():
    assert cglobal("austin_errno", "int") == 0
