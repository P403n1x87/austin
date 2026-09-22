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

from functools import partial
from pathlib import Path
from test.utils import allpythons as _allpythons
from test.utils import austin
from test.utils import python
from test.utils import requires_sudo
from test.utils import retry_where
from test.utils import run_python
from test.utils import target

from flaky import flaky

# Every test in this module is asyncio task-graph introspection, 3.14+ only.
allpythons = partial(_allpythons, min=(3, 14))


@requires_sudo
@allpythons()
def test_where_asyncio_task_tree(py):
    with run_python(py, target("target_asyncio.py")) as p:
        expected = (
            "worker-0",
            "worker-1",
            "pipeline_stage",
            "process_item",
            "fetch_data",
            "leaf_work",
            "sleep",
        )
        result = retry_where(p.pid, lambda out: all(s in out for s in expected))
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
@allpythons()
def test_where_asyncio_multiloop(py):
    """Each thread's own event loop shows only its own tasks.

    Regression test: the task tree used to be built into one buffer shared
    across the whole sample, gated by a single "already printed" flag, so
    only the first loop found was ever rendered -- every other thread's
    tasks were captured but silently dropped, not merely duplicated.
    """
    with run_python(py, target("target_asyncio_multiloop.py")) as p:
        expected = (
            "loop0-worker-0",
            "loop0-worker-1",
            "loop1-worker-0",
            "loop1-worker-1",
        )
        result = retry_where(p.pid, lambda out: all(s in out for s in expected))
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


@requires_sudo
@allpythons()
def test_where_asyncio_fan_in(py):
    """Two independent tasks directly awaiting the same third task at once
    forces CPython to represent that task's task_awaited_by as a SET rather than
    a single reference -- the only way to exercise Austin's waiter-set walk, as
    opposed to the single-waiter fast path every other asyncio test target
    exercises. Rendered as a tree, a task with two waiters necessarily shows up
    twice (once nested under each waiter, since a fan-in can't be flattened into
    a single-parent tree) -- that duplication is the signal that both waiter
    edges from the set were actually walked and emitted."""
    with run_python(py, target("target_asyncio_fanin.py")) as p:
        expected = ("shared_worker", "fan-in-0", "fan-in-1")
        result = retry_where(
            p.pid,
            lambda out: (
                all(s in out for s in expected) and out.count("shared_worker") >= 2
            ),
        )
        assert result.returncode == 0

        out = result.stdout

        assert "fan-in-0" in out
        assert "fan-in-1" in out
        # shared_worker must appear once per waiter -- if the waiter set was
        # only partially walked (or treated as a single reference), it would
        # show up just once.
        assert out.count("shared_worker") >= 2, out


@flaky
@requires_sudo
@allpythons()
def test_where_asyncio_orphaned_task(py):
    """A task whose owning thread died while it was still referenced shows
    up in the "Orphaned task tree" fallback section rather than being
    silently dropped."""
    with run_python(py, target("target_asyncio_orphan.py")) as p:
        result = retry_where(
            p.pid, lambda out: "Orphaned task tree" in out and "stuck_worker" in out
        )
        assert result.returncode == 0

        out = result.stdout

        assert "Orphaned task tree" in out
        assert "stuck_worker" in out


@allpythons()
def test_asyncio_mojo_smoke(py, tmp_path: Path):
    """Continuous MOJO-format sampling with asyncio task scanning active
    completes cleanly and produces non-empty output. See the module
    docstring for why this doesn't assert on the task events themselves.

    Runs with -P (pipe mode) so a task-stack event's own extra fflush (see
    mojo_event_handler__handle_task_stack_end in events.c) actually fires at
    least once -- every other asyncio test target uses plain file output,
    which never takes that branch."""
    datafile = tmp_path / "test_asyncio_mojo.austin"

    result = austin(
        "-i",
        "1000",
        "-P",
        "-o",
        str(datafile),
        *python(py),
        target("target_asyncio.py"),
    )
    assert result.returncode == 0, result.stderr or result.stdout

    assert datafile.stat().st_size > 0


@allpythons()
def test_native_asyncio_disabled(py, save_mojo):
    """Asyncio task-graph scanning is disabled outright in native mode (-n).

    On at least one real-world build (GitHub Actions' hosted CPython 3.14.7,
    GCC 13.3.0), py_thread__split_task_stack_at's native-mode EVAL_FRAME_MAGIC/
    CFRAME_MAGIC pairing didn't just fail to activate -- it found a
    plausible-looking but WRONG split point, moving the thread's own frames
    into a task's report and corrupting the thread's own remainder. That's
    silent data corruption, not a missed enhancement, so
    _py_proc__maybe_discover_asyncio (py_proc.c) never even looks for the
    AsyncioDebug section while pargs_native, which keeps self->
    asyncio_debug_found false and every task-graph call site in
    _py_proc__sample_threads a no-op. See project_py315_support.md.

    Confirms no task data is ever emitted in native mode, and that we're
    still getting real, sensible stacks from the target script otherwise --
    but deliberately checks Handle._run, not cpu_burst. cpu_burst sits
    right after the point where task-stepping crosses into a
    contextvars.Context.run() call, and that crossing is a separate,
    pre-existing, general native-mode limitation independent of task-graph
    scanning: it reproduces even with scanning fully disabled (as here),
    and on builds that never touched py_thread__split_task_stack_at's logic
    at all -- e.g. free-threaded 3.14t on real Linux CI hardware, no
    emulation involved. Matches the "nested eval loops (context_run)
    exhaust the Python stack at the wrong sentinel" issue already tracked
    in project_py315_support.md, now confirmed to also affect non-3.15
    builds. Handle._run is the last frame *before* that crossing, so it's
    unaffected and still a meaningful check that sampling is working."""
    result = austin("-n", "-i", "1000", *python(py), target("target_asyncio_cpu.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    assert not any(
        sample.tasks for sample in result.samples
    ), "Expected no task data in native mode"

    assert any(
        frame.function == "Handle._run"
        for sample in result.samples
        for frame in (sample.frames or ())
    ), "Expected Handle._run in the thread's own frames"


def _iter_tasks(tasks):
    for t in tasks:
        yield t
        yield from _iter_tasks(t.awaiting)


@allpythons()
def test_asyncio_executing_task_survives_repeat_samples(py, save_mojo):
    """An EXECUTING task's dwell time is never dropped just because the
    thread running it produced repeat samples (top frame/code/lasti
    unchanged since the last tick -- see stack_py_find_origin's own doc
    comment on PYSTACK_REPEAT_MAGIC).

    _py_asyncio__emit_task (py_asyncio.c) splits a task's frames off its
    owning thread's stack every tick it's caught EXECUTING. A repeat
    sample's stack holds nothing but the REPEAT sentinel, so the split can
    never succeed -- it used to just silently drop that tick's sample. A
    long synchronous/CPU-bound stretch inside a coroutine produces many
    repeat samples in a row (that's exactly what "repeat" means: nothing
    about the top frame changed between ticks), so this could lose a large
    fraction of a task's true on-CPU time with no signal anything was
    wrong. Fixed via task_tracker_entry_t.executing_time, which accumulates
    dwell time across repeat ticks and flushes it in one shot once the
    position finally changes. See project_asyncio_task_split_repeat.md.

    target_asyncio_blocked.py uses a real time.sleep() rather than a CPU
    loop to make repeat samples certain rather than merely likely: a
    blocking call freezes the calling frame's code+lasti for its whole
    duration, deterministically, on any machine at any speed -- unlike a
    CPU-bound loop, which only makes repeats statistically probable and
    would make this test flaky. Its second call (marker_call) exists purely
    to force a state change that flushes the first call's accumulated
    time while austin is still actively sampling, rather than relying on
    the process happening to exit at just the right moment.

    Asserts on the accumulated elapsed time of the flush that follows the
    long repeat stretch: at most a couple of milliseconds if ticks are
    still being dropped (the pre-fix behaviour), several hundred
    milliseconds if they're correctly accumulated (the fix). 200ms is a
    threshold with a wide margin on both sides of that gap, chosen well
    below the real ~500ms dwell so ordinary CI scheduling variance can't
    flip the result -- this doesn't depend on how many samples land during
    the sleep, only on the wall-clock duration of the sleep itself, which
    is fixed regardless of machine speed."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_blocked.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    max_elapsed = max(
        (
            task.elapsed or 0
            for sample in result.samples
            for task in _iter_tasks(sample.tasks or ())
        ),
        default=0,
    )

    assert max_elapsed > 200_000, (
        "Expected a task's accumulated EXECUTING dwell time across a long "
        "repeat-sample stretch to be flushed in one shot (>200ms); got at "
        f"most {max_elapsed}us -- repeat-sample ticks may be getting "
        "silently dropped again (see project_asyncio_task_split_repeat.md)"
    )
