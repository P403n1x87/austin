import os

import test.cunit.env as env


def test_parse_env():
    assert env.parse_env() == 0

    os.environ["AUSTIN_NO_LOGGING"] = "1"
    assert env.parse_env() == 0


def test_parse_env_invalid():
    os.environ["AUSTIN_PAGE_SIZE_CAP"] = "invalid"
    assert env.parse_env() != 0
