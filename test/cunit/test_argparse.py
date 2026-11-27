from ctypes import Structure
from ctypes import c_bool
from ctypes import c_char_p
from ctypes import c_int
from ctypes import c_uint64
from ctypes import c_void_p

import pytest

import test.cunit.argparse as argparse


def parse_args(argv):
    argc = len(argv)
    return argparse.parse_args(argc, (c_char_p * argc)(*(_.encode() for _ in argv)))


class ParsedArgs(Structure):
    # Leading fields of parsed_args_t, up to the exposure. The field following
    # attach_pid is pointer-aligned, so the layout does not depend on the size
    # of pid_t, and reading it as an int is fine for small PIDs on the
    # little-endian hosts the unit tests run on.
    _fields_ = [
        ("t_sampling_interval", c_uint64),
        ("timeout", c_uint64),
        ("attach_pid", c_int),
        ("cmd", c_void_p),
        ("where", c_bool),
        ("cpu", c_bool),
        ("full", c_bool),
        ("memory", c_bool),
        ("output_file", c_void_p),
        ("output_filename", c_void_p),
        ("children", c_bool),
        ("exposure", c_uint64),
    ]


def pargs():
    return ParsedArgs.in_dll(argparse.__binary__, "pargs")


def test_parse_args_command():
    parse_args(["austin", "python"])


@pytest.mark.exitcode(64)
def test_parse_args_no_target():
    parse_args(["austin", "-1", "100"])


def test_parse_args_process():
    parse_args(["austin", "-p", "123"])
    assert pargs().attach_pid == 123
    assert not pargs().where


def test_parse_args_where():
    parse_args(["austin", "-w", "123"])
    assert pargs().attach_pid == 123
    assert pargs().where


def test_parse_args_exposure():
    parse_args(["austin", "-x", "42", "-p", "123"])
    assert pargs().exposure == 42


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
    assert pargs().t_sampling_interval == expected


@pytest.mark.parametrize(
    "value,expected",
    [("500", 500_000), ("500ms", 500_000), ("2s", 2_000_000)],
)
def test_parse_args_timeout_units(value, expected):
    parse_args(["austin", "-t", value, "-p", "123"])
    assert pargs().timeout == expected


@pytest.mark.exitcode(64)
def test_parse_args_interval_unit_overflow():
    parse_args(["austin", "-i", "9223372036854775807s", "-p", "123"])


@pytest.mark.exitcode(64)
@pytest.mark.parametrize("pid", ["-123", "99999999999999999999", "123 "])
def test_parse_args_invalid_pid_value(pid):
    parse_args(["austin", "-p", pid])


@pytest.mark.exitcode(64)
@pytest.mark.parametrize("value", ["-1", "-1ms", "99999999999999999999"])
def test_parse_args_invalid_interval_value(value):
    parse_args(["austin", "-i", value, "-p", "123"])


@pytest.mark.exitcode(64)
@pytest.mark.parametrize("value", ["-1", "-1s", "9223372036854775807"])
def test_parse_args_invalid_timeout_value(value):
    parse_args(["austin", "-t", value, "-p", "123"])


@pytest.mark.exitcode(64)
@pytest.mark.parametrize("value", ["5s", "99999999999999999999"])
def test_parse_args_invalid_exposure_value(value):
    parse_args(["austin", "-x", value, "-p", "123"])
