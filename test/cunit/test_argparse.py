from ctypes import c_char_p
import sys

import pytest

import test.cunit.argparse as argparse


def parse_args(argv):
    argc = len(argv)
    return argparse.parse_args(argc, (c_char_p * argc)(*(_.encode() for _ in argv)))


def test_parse_args_command():
    parse_args(["austin", "python"])


@pytest.mark.exitcode(64 if sys.platform == "linux" else 1)
def test_parse_args_no_target():
    parse_args(["austin", "-1", "100"])


def test_parse_args_process():
    parse_args(["austin", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_non_numeric_process_id():
    parse_args(["austin", "-p", "abc123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_process_id():
    parse_args(["austin", "-p", "0"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_timeout():
    parse_args(["austin", "-t", "abc123", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_interval():
    parse_args(["austin", "-i", "abc123", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_interval_unit():
    parse_args(["austin", "-i", "123jiffy", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_interval_unit_seconds():
    parse_args(["austin", "-i", "123sm", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_interval_unit_milliseconds():
    parse_args(["austin", "-i", "123msm", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_interval_unit_microseconds():
    parse_args(["austin", "-i", "123usm", "-p", "123"])


@pytest.mark.exitcode(64 if sys.platform == "linux" else 1)
def test_parse_args_invalid_exposure():
    parse_args(["austin", "-x", "-1", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_timeout_unit():
    parse_args(["austin", "-t", "123jiffy", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_timeout_unit_seconds():
    parse_args(["austin", "-t", "123sm", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_timeout_unit_milliseconds():
    parse_args(["austin", "-t", "123msm", "-p", "123"])


@pytest.mark.exitcode(64)
def test_parse_args_invalid_where_pid():
    parse_args(["austin", "-w", "123asdf"])


@pytest.mark.exitcode(64)
def test_parse_args_pid_and_command():
    parse_args(["austin", "-p", "123", "python"])
