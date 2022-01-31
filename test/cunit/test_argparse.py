import test.cunit.argparse as argparse
from ctypes import c_char_p

import pytest


def parse_args(argv):
    argc = len(argv)
    return argparse.parse_args(argc, (c_char_p * argc)(*(_.encode() for _ in argv)))


def test_parse_args_command():
    assert parse_args(["austin", "python"])


# FIXME
@pytest.mark.exitcode(1)
def test_parse_args_process():
    assert parse_args(["austin", "-p", "123"])
