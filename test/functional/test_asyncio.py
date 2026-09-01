# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2026 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

from pathlib import Path
from test.utils import allpythons
from test.utils import austin
from test.utils import python
from test.utils import requires_sudo
from test.utils import run_python
from test.utils import target

from flaky import flaky
import pytest


@requires_sudo
@allpythons(min=(3, 14))
def test_where_asyncio_task_tree(py):
    with run_python(py, target("target_asyncio.py"), sleep_after=2) as p:
        result = austin("-w", str(p.pid))
        assert result.returncode == 0

        out = result.stdout

        assert "Process" in out
        assert "Thread" in out

        # Both sibling tasks show up, each nested under the root task...
        assert "worker-0" in out
        assert "worker-1" in out

        # ...with the deep await chain resolved all the way to the leaf.
        assert "pipeline_stage" in out
        assert "process_item" in out
        assert "fetch_data" in out
        assert "leaf_work" in out
        assert "sleep" in out


@requires_sudo
@allpythons(min=(3, 14))
def test_where_asyncio_multiloop(py):
    """Each thread's own event loop shows only its own tasks.

    Regression test: the task tree used to be built into one buffer shared
    across the whole sample, gated by a single "already printed" flag, so
    only the first loop found was ever rendered -- every other thread's
    tasks were captured but silently dropped, not merely duplicated.
    """
    with run_python(py, target("target_asyncio_multiloop.py"), sleep_after=1) as p:
        result = austin("-w", str(p.pid))
        assert result.returncode == 0

        out = result.stdout

        # Split into one chunk of output per thread, keyed by thread name.
        # "🧵 Thread " only appears on the per-thread header line (see
        # WHERE_HEAD_FORMAT in events.c) -- a plain "Thread" substring check
        # would also match frame lines like "Thread._bootstrap (...)" from
        # threading.py itself.
        chunks = {}
        current = None
        for line in out.splitlines():
            if "🧵 Thread " in line:
                current = line
                chunks[current] = []
            elif current is not None:
                chunks[current].append(line)

        loop0_chunk = next(v for k, v in chunks.items() if "Loop0-Thread" in k)
        loop1_chunk = next(v for k, v in chunks.items() if "Loop1-Thread" in k)

        loop0_text = "\n".join(loop0_chunk)
        loop1_text = "\n".join(loop1_chunk)

        assert "loop0-worker-0" in loop0_text
        assert "loop0-worker-1" in loop0_text
        assert "loop1-worker-0" not in loop0_text
        assert "loop1-worker-1" not in loop0_text

        assert "loop1-worker-0" in loop1_text
        assert "loop1-worker-1" in loop1_text
        assert "loop0-worker-0" not in loop1_text
        assert "loop0-worker-1" not in loop1_text


@flaky
@requires_sudo
@allpythons(min=(3, 14))
def test_where_asyncio_orphaned_task(py):
    """A task whose owning thread died while it was still referenced shows
    up in the "Orphaned task tree" fallback section rather than being
    silently dropped."""
    with run_python(py, target("target_asyncio_orphan.py"), sleep_after=2) as p:
        result = austin("-w", str(p.pid))
        assert result.returncode == 0

        out = result.stdout

        assert "Orphaned task tree" in out
        assert "stuck_worker" in out


@allpythons(min=(3, 14))
def test_asyncio_mojo_smoke(py, tmp_path: Path):
    """Continuous MOJO-format sampling with asyncio task scanning active
    completes cleanly and produces non-empty output. See the module
    docstring for why this doesn't assert on the task events themselves."""
    datafile = tmp_path / "test_asyncio_mojo.austin"

    result = austin(
        "-i", "1000", "-o", str(datafile), *python(py), target("target_asyncio.py")
    )
    assert result.returncode == 0, result.stderr or result.stdout

    assert datafile.stat().st_size > 0
