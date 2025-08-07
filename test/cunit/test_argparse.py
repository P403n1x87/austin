import test.cunit.argparse as argparse
from ctypes import c_char_p

import pytest


def parse_args(argv):
    argc = len(argv)
    return argparse.parse_args(argc, (c_char_p * argc)(*(_.encode() for _ in argv)))


def test_parse_args_command():
    parse_args(["austin", "python"])


def test_parse_args_process():
    parse_args(["austin", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_process_id():
    parse_args(["austin", "-p", "abc123"])
