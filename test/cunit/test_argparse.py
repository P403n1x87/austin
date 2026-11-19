from ctypes import c_char_p
from ctypes import c_uint64

import pytest

import test.cunit.argparse as argparse


def parse_args(argv):
    argc = len(argv)
    return argparse.parse_args(argc, (c_char_p * argc)(*(_.encode() for _ in argv)))


def parsed_times():
    # The first two fields of parsed_args_t are the sampling interval and the
    # timeout, both 64-bit unsigned integers.
    interval, timeout = (c_uint64 * 2).in_dll(argparse.__binary__, "pargs")
    return interval, timeout


def test_parse_args_command():
    parse_args(["austin", "python"])


@pytest.mark.exitcode(64)
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


@pytest.mark.exitcode(64)
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


@pytest.mark.parametrize(
    "value,expected",
    [("250", 250), ("250us", 250), ("5ms", 5_000), ("2s", 2_000_000)],
)
def test_parse_args_interval_units(value, expected):
    parse_args(["austin", "-i", value, "-p", "123"])
    assert parsed_times()[0] == expected


@pytest.mark.parametrize(
    "value,expected",
    [("500", 500_000), ("500ms", 500_000), ("2s", 2_000_000)],
)
def test_parse_args_timeout_units(value, expected):
    parse_args(["austin", "-t", value, "-p", "123"])
    assert parsed_times()[1] == expected


@pytest.mark.exitcode(64)
def test_parse_args_interval_unit_overflow():
    parse_args(["austin", "-i", "9223372036854775807s", "-p", "123"])
