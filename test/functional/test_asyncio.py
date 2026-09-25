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


def _iter_task_stack_blocks(data: bytes):
    """Yield (task_id, is_closing) for every MOJO_TASK_STACK block, in wire
    order. is_closing is True iff the block carried zero frames (Austin's
    "this task is gone" signal). Deliberately independent of AustinTask/
    AustinSample: those collapse a closing block into an elapsed-time
    update on the existing task, with no trace of how many closing blocks
    were seen or where -- exactly the distinction the test below needs.

    A minimal, full walk of every MOJO event type is needed (not just the
    ones directly relevant here) since each carries a different,
    fixed-shape payload that must be skipped correctly to stay aligned --
    getting even one wrong (e.g. MOJO_STRING_REF's own varint) desyncs
    everything that follows."""
    (
        METADATA,
        STACK,
        FRAME,
        FRAME_INVALID,
        FRAME_REF,
        FRAME_KERNEL,
        GC,
        IDLE,
        METRIC_TIME,
        METRIC_MEMORY,
        STRING,
        STRING_REF,
        STACK_REPEAT,
        TASK_STACK,
        TASK_WAITER,
    ) = range(1, 16)

    pos = 3
    pos += _varint_len(data, pos)  # header varint, value unused here

    in_block = False
    frame_count = 0
    task_id = None

    while pos < len(data):
        ev = data[pos]
        pos += 1
        if ev == METADATA:
            pos = data.index(b"\0", pos) + 1
            pos = data.index(b"\0", pos) + 1
        elif ev == STACK:
            in_block = False
            for _ in range(2):
                pos += _varint_len(data, pos)
            pos = data.index(b"\0", pos) + 1
        elif ev == FRAME:
            _, n = _varint(data, pos)  # key, unused here
            pos += n
            for _ in range(6):  # fk, sk, line, line_end, column, column_end
                pos += _varint_len(data, pos)
            if in_block:
                frame_count += 1
        elif ev == FRAME_REF:
            pos += _varint_len(data, pos)
            if in_block:
                frame_count += 1
        elif ev == FRAME_KERNEL:
            pos = data.index(b"\0", pos) + 1
        elif ev in (FRAME_INVALID, GC, IDLE):
            pass
        elif ev == METRIC_TIME:
            pos += _varint_len(data, pos)
            if in_block:
                yield (task_id, frame_count == 0)
                in_block = False
        elif ev == METRIC_MEMORY:
            pos += _varint_len(data, pos)
        elif ev == STRING:
            pos += _varint_len(data, pos)
            pos = data.index(b"\0", pos) + 1
        elif ev == STRING_REF:
            pos += _varint_len(data, pos)
        elif ev == STACK_REPEAT:
            in_block = False
        elif ev == TASK_STACK:
            task_id, n = _varint(data, pos)
            pos += n
            pos += _varint_len(data, pos)  # name_key, unused here
            in_block = True
            frame_count = 0
        elif ev == TASK_WAITER:
            for _ in range(2):
                pos += _varint_len(data, pos)
        else:
            raise ValueError(f"Unknown MOJO event {ev} at offset {pos - 1}")


def _varint(data: bytes, pos: int):
    """Decode one MOJO varint at pos; return (value, bytes_consumed)."""
    b0 = data[pos]
    value = b0 & 0x3F
    shift = 6
    n = 1
    cont = (b0 >> 7) & 1
    while cont:
        b = data[pos + n]
        value |= (b & 0x7F) << shift
        shift += 7
        n += 1
        cont = (b >> 7) & 1
    return value, n


def _varint_len(data: bytes, pos: int) -> int:
    return _varint(data, pos)[1]


def _iter_tasks_flat(tasks):
    for t in tasks:
        yield t
        yield from _iter_tasks_flat(t.awaiting)


def _is_asyncio_shutdown_task(t):
    """True for the transient task asyncio.run() schedules for its own
    post-main() cleanup (BaseEventLoop.shutdown_asyncgens) -- a real,
    separate task unrelated to the pyramid's own intentional structure.
    A long enough sampling window's tail occasionally catches it for a
    sample or two once main() has already returned, which is not a
    fragmentation bug: exclude it so the task count isn't sensitive to
    exactly when sampling stops relative to interpreter shutdown."""
    return bool(t.frames) and t.frames[0].function == "BaseEventLoop.shutdown_asyncgens"


@allpythons()
def test_asyncio_no_ambiguous_task_close_signal(py, save_mojo):
    """A task's dwell time while briefly caught EXECUTING mid-await is
    folded into its own accumulator (see task_tracker.h's executing_time)
    rather than flushed as a standalone empty-frame MOJO_TASK_STACK -- the
    wire's only "this task is gone" signal, otherwise indistinguishable
    from _py_asyncio__emit_task's real end-of-life eviction flush (see
    py_asyncio__scan_tasks_end and its own merged single-flush fix).

    A consumer using that signal to track task identity/generations (a
    reasonable thing to do -- it's the only "this position is done" signal
    the wire gives) would misread an ordinary mid-life SUSPENDED->EXECUTING
    ->SUSPENDED transition as the task dying, fragmenting one real,
    continuously-alive task into several apparent ones. Confirmed exactly
    this way against austin-vscode: a single root task appeared as several
    unrelated "root" tasks after being caught executing partway through its
    own await chain.

    target_asyncio_pyramids.py's own outer task (driving pyramid_loop)
    reliably gets caught EXECUTING at least once per round -- unlike the
    address-reuse race in project_asyncio_task_split_repeat.md, this isn't
    a rare timing coincidence: every round does real, non-trivial work
    (asyncio.create_task(...) plus bookkeeping) synchronously between two
    awaits, and 1ms sampling over that reliably catches it.

    _is_asyncio_shutdown_task filters out BaseEventLoop.shutdown_asyncgens
    -- a real, separate task asyncio.run() schedules for its own cleanup
    once main() returns, which a long enough sampling window's tail can
    occasionally catch. Confirmed by decoding real CI captures where the
    count came back 30: the extra id was always this exact task, appearing
    only in the last handful of samples -- not a fragmented pyramid task.

    Two complementary checks against a single real capture:

    - Real, decoded data (the same AustinTask/AustinSample model every
      other test in this file uses): the pyramid's own shape has exactly
      one true root (main's implicit task; every subtask is directly
      awaited by something) and exactly 29 tasks total -- 1 root + 4
      depth-2 round subtasks (ROUNDS) + 8 depth-1 children (ROUNDS *
      SUB_ROUNDS) + 16 depth-0 leaves (ROUNDS * SUB_ROUNDS * SUB_ROUNDS --
      depth-1 tasks also each spawn SUB_ROUNDS children of their own, they
      just don't recurse further since depth 0 returns immediately).
      Fragmenting one task into several apparent ones from either bug this
      test guards -- the ambiguous close signal above, or the address-reuse
      id collision in task_tracker.h's epoch -- would inflate one or both
      of these counts.

    - The raw wire (see _iter_task_stack_blocks): AustinTask collapses a
      closing (empty-frame) block into an elapsed-time update with no
      trace of how many were seen, so it can't see the specific defect
      fixed here -- a task evicted with both suspended and executing dwell
      pending used to flush TWO closing blocks back to back. For every
      task id, an empty-frame block may only be the LAST block."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_pyramids.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    all_tasks = [
        t
        for sample in result.samples
        for t in _iter_tasks_flat(sample.tasks or ())
        if not _is_asyncio_shutdown_task(t)
    ]
    distinct_ids = {t.task_id for t in all_tasks}
    assert len(distinct_ids) == 29, (
        f"Expected exactly 29 distinct tasks (1 root + 4 depth-2 + 8 depth-1 "
        f"+ 16 depth-0); got {len(distinct_ids)}: {sorted(distinct_ids)} "
        "-- extra ids mean a task got fragmented into several apparent ones"
    )

    root_ids = {
        t.task_id
        for sample in result.samples
        for t in (sample.tasks or ())
        if not _is_asyncio_shutdown_task(t)
    }
    assert len(root_ids) == 1, (
        f"Expected a single root task throughout the whole capture; got "
        f"{len(root_ids)}: {sorted(root_ids)} -- a spurious extra root means "
        "a mid-life transition was mistaken for that task dying"
    )

    raw = (
        result.stdout
        if isinstance(result.stdout, (bytes, bytearray))
        else result.stdout.encode()
    )

    last_was_closing = {}
    violations = []
    for task_id, is_closing in _iter_task_stack_blocks(raw):
        if last_was_closing.get(task_id):
            violations.append(task_id)
        last_was_closing[task_id] = is_closing

    assert violations == [], (
        "Expected an empty-frame (closing) MOJO_TASK_STACK to only ever be "
        f"the LAST block for its task id; task id(s) {sorted(set(violations))} "
        "had another block follow a closing one, meaning a single task's "
        "end-of-life dwell was split across more than one closing signal"
    )
