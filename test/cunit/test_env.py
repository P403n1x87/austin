import os

import test.cunit.env as env


def test_parse_env():
    assert env.parse_env() == 0

    os.environ["AUSTIN_NO_LOGGING"] = "1"
    assert env.parse_env() == 0


def test_parse_env_invalid():
    os.environ["AUSTIN_PAGE_SIZE_CAP"] = "invalid"
    assert env.parse_env() != 0


def test_env_get():
    os.environ["AUSTIN_TEST_VAR"] = "value"
    assert env.env_get(b"AUSTIN_TEST_VAR") == b"value"

    os.environ["AUSTIN_TEST_VAR"] = ""
    assert env.env_get(b"AUSTIN_TEST_VAR") is None

    del os.environ["AUSTIN_TEST_VAR"]
    assert env.env_get(b"AUSTIN_TEST_VAR") is None


def test_env_lookup():
    block = b"PATH=/usr/bin\0VIRTUAL_ENV_PROMPT=(venv)\0VIRTUAL_ENV=/opt/venv\0EMPTY=\0"

    def lookup(name):
        return env.env_lookup(block, len(block) - 1, name)

    assert lookup(b"VIRTUAL_ENV") == b"/opt/venv"
    assert lookup(b"PATH") == b"/usr/bin"
    assert lookup(b"VIRTUAL") is None
    assert lookup(b"EMPTY") is None
    assert lookup(b"MISSING") is None


def test_env_lookup_stops_at_empty_entry():
    # Anything past an empty entry is not part of the environment.
    block = b"A=1\0\0B=2\0"
    assert env.env_lookup(block, len(block) - 1, b"A") == b"1"
    assert env.env_lookup(block, len(block) - 1, b"B") is None
