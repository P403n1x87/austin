# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
# All rights reserved.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

import platform
from test.utils import allpythons
from test.utils import austin
from test.utils import has_pattern
from test.utils import metadata
from test.utils import mojo
from test.utils import python
from test.utils import samples
from test.utils import target

import pytest


@allpythons(min=(3, 7))
@mojo
def test_gc_off(py, mojo):
    result = austin("-i", "1ms", *python(py), target("target_gc.py"), mojo=mojo)
    assert result.returncode == 0

    assert not has_pattern(":GC:", result.stdout)


@pytest.mark.xfail(
    platform.system() != "Linux",
    reason="GC sampling seems to work reliably only on Linux",
)
@allpythons(min=(3, 7))
@mojo
def test_gc_on(py, mojo):
    result = austin("-gi", "1ms", *python(py), target("target_gc.py"), mojo=mojo)
    assert result.returncode == 0

    meta = metadata(result.stdout)
    assert float(meta["gc"]) / float(meta["duration"]) > 0.1

    gcs = [_ for _ in samples(result.stdout) if ":GC:" in _]
    assert len(gcs) > 10


@allpythons(min=(3, 7))
@mojo
def test_gc_disabled(py, monkeypatch, mojo):
    monkeypatch.setenv("GC_DISABLED", "1")

    result = austin("-gi", "10ms", *python(py), target("target_gc.py"), mojo=mojo)
    assert result.returncode == 0

    meta = metadata(result.stdout)
    assert int(meta["gc"]) * 0.8 < int(meta["duration"]) / 20

    gcs = [_ for _ in samples(result.stdout) if ":GC:" in _]
    assert len(gcs) < 5
