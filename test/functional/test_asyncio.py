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
from typing import NamedTuple
from typing import Optional
from typing import Tuple
from test.utils import allpythons as _allpythons
from test.utils import austin
from test.utils import python
from test.utils import requires_sudo
from test.utils import retry_where
from test.utils import run_python
from test.utils import target

import pytest
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
    """A running task is credited the time of every sample it runs at,
    repeat samples included.

    A long synchronous stretch inside a coroutine produces many repeat
    samples in a row (nothing about the top frame changes between them), on
    which the task's frames can't be split off the thread's stack. Its
    running time used to be carried by task reports, so it had to be held
    back across that stretch, and could be lost. It is now the time of the
    samples whose thread reports the task as running (MOJO_TASK_RUNNING),
    which is emitted on repeat samples too.

    target_asyncio_blocked.py uses a real time.sleep() rather than a CPU
    loop to make repeat samples certain rather than merely likely: a
    blocking call freezes the calling frame's code+lasti for its whole
    duration, deterministically, on any machine at any speed. 200ms is well
    below its real ~500ms stretch, so ordinary CI scheduling variance can't
    flip the result, and it doesn't depend on how many samples land during
    the sleep, only on its wall-clock duration."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_blocked.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    running = _running_time_by_task(_raw_mojo(result))
    max_running = max(running.values(), default=0)

    assert max_running > 200_000, (
        "Expected a task's running time across a long repeat-sample stretch "
        f"to be counted in full (>200ms); got at most {max_running}us"
    )


class SampleRecord(NamedTuple):
    time: int
    idle: bool
    running: Optional[int]  # id of the task the thread was running, if any


class TaskStackBlock(NamedTuple):
    task_id: int
    name: Optional[str]
    scopes: Tuple[str, ...]  # empty for a closing block


def _iter_mojo_records(data: bytes):
    """Yield, in wire order, a TaskStackBlock for every MOJO_TASK_STACK
    block, and a SampleRecord for every thread sample. A task
    block with no frames is Austin's "this task is gone" signal.
    Deliberately independent of AustinTask/AustinSample, which build task
    trees and work out task times from these: the tests below check the
    wire itself.

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
        TASK_RUNNING,
    ) = range(1, 17)

    pos = 3
    pos += _varint_len(data, pos)  # header varint, value unused here

    strings = {}
    frame_scopes = {}

    in_block = False
    scopes = []
    task_id = None
    name_key = 0
    frame_count = 0
    sample_idle = False
    sample_running = None
    # A sample's time metric isn't its last event (in full mode the idle flag
    # follows it), so it is only yielded once the sample is over.
    sample_time = None

    while pos < len(data):
        ev = data[pos]
        pos += 1
        if ev == METADATA:
            pos = data.index(b"\0", pos) + 1
            pos = data.index(b"\0", pos) + 1
        elif ev == STACK:
            if sample_time is not None:
                yield SampleRecord(sample_time, sample_idle, sample_running)
            in_block = False
            sample_idle = False
            sample_running = None
            sample_time = None
            for _ in range(2):
                pos += _varint_len(data, pos)
            pos = data.index(b"\0", pos) + 1
        elif ev == FRAME:
            key, n = _varint(data, pos)
            pos += n
            pos += _varint_len(data, pos)  # filename key, unused here
            scope_key, n = _varint(data, pos)
            pos += n
            for _ in range(4):  # line, line_end, column, column_end
                pos += _varint_len(data, pos)
            frame_scopes[key] = strings.get(scope_key)
            if in_block:
                scopes.append(frame_scopes[key])
        elif ev == FRAME_REF:
            key, n = _varint(data, pos)
            pos += n
            if in_block:
                scopes.append(frame_scopes.get(key))
                if len(scopes) == frame_count:
                    name = strings.get(name_key) if name_key else None
                    yield TaskStackBlock(task_id, name, tuple(scopes))
                    in_block = False
        elif ev == FRAME_KERNEL:
            pos = data.index(b"\0", pos) + 1
        elif ev == IDLE:
            sample_idle = True
        elif ev in (FRAME_INVALID, GC):
            pass
        elif ev == METRIC_TIME:
            sample_time, n = _varint(data, pos)
            pos += n
        elif ev == METRIC_MEMORY:
            pos += _varint_len(data, pos)
        elif ev == STRING:
            key, n = _varint(data, pos)
            pos += n
            end = data.index(b"\0", pos)
            strings[key] = data[pos:end].decode()
            pos = end + 1
        elif ev == STRING_REF:
            pos += _varint_len(data, pos)
        elif ev == STACK_REPEAT:
            in_block = False
        elif ev == TASK_STACK:
            task_id, n = _varint(data, pos)
            pos += n
            name_key, n = _varint(data, pos)
            pos += n
            frame_count, n = _varint(data, pos)
            pos += n
            scopes = []
            # A report is its next frame_count frame references; none closes
            # the task.
            if frame_count:
                in_block = True
            else:
                yield TaskStackBlock(task_id, None, ())
        elif ev == TASK_RUNNING:
            sample_running, n = _varint(data, pos)
            pos += n
        elif ev == TASK_WAITER:
            for _ in range(2):
                pos += _varint_len(data, pos)
        else:
            raise ValueError(f"Unknown MOJO event {ev} at offset {pos - 1}")

    if sample_time is not None:
        yield SampleRecord(sample_time, sample_idle, sample_running)


def _iter_task_stack_blocks(data: bytes):
    return (r for r in _iter_mojo_records(data) if isinstance(r, TaskStackBlock))


def _iter_samples(data: bytes):
    return (r for r in _iter_mojo_records(data) if isinstance(r, SampleRecord))


def _iter_sample_times(data: bytes):
    return (r.time for r in _iter_samples(data))


def _running_time_by_task(data: bytes, cpu_only: bool = False):
    """Time of the samples each task was running at -- its running time. In
    full mode, cpu_only keeps the samples whose thread wasn't idle."""
    times = {}
    for r in _iter_samples(data):
        if r.running is not None and not (cpu_only and r.idle):
            times[r.running] = times.get(r.running, 0) + r.time
    return times


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


def _raw_mojo(result) -> bytes:
    return (
        result.stdout
        if isinstance(result.stdout, (bytes, bytearray))
        else result.stdout.encode()
    )


def _iter_tasks_flat(tasks):
    for t in tasks:
        yield t
        yield from _iter_tasks_flat(t.awaiting)


def _transient_root_ids(samples, max_fraction=0.01):
    """Task ids whose apparent "root" status is a sampling artifact, not a
    fragmentation bug: a task's TASK_STACK identity can land on the wire
    before its TASK_WAITER edge -- either a torn read of task_awaited_by
    (retried once on first sighting, see _py_asyncio__emit_task) or a
    genuine gap between a task being created and something actually
    awaiting it. This resolves itself as soon as a later tick reads the
    now-set edge, which produces two different-looking but equally benign
    shapes in a finite capture, both confined to a small fraction of the
    whole capture (confirmed against real CI/local captures: single- and
    double-digit sample counts out of several thousand):

    - Mid-capture flicker: the id has OTHER, later samples where it's
      correctly parented -- proof it's a real, stable, singly-identified
      task that merely flickered, not a count-inflating duplicate.
      Excluded from the single-root check only; it must still count
      toward the distinct-task total, since it unambiguously is one.
    - Unresolved tail: the id is root for every single sample it ever
      appeared in, ending exactly at the capture's last sample -- the gap
      never closes because there's no further tick left to observe the
      edge. Genuinely indeterminate (a legitimate task simply created
      late, or an actual fragmentation duplicate -- the capture ending
      before any parent is ever seen makes the two indistinguishable), so
      excluded from both checks rather than guessed either way.

    Returns (root_only, full): disjoint sets for the two shapes above. A
    real fragmentation bug -- two ids for what's actually one task --
    doesn't reliably take either of these shapes (a duplicate id's root
    time isn't confined to a tiny fraction of an otherwise-normal
    lifetime, nor to a short run landing exactly on the capture's last
    sample), so this doesn't launder that; it only excuses the two
    provably-benign shapes this sampling race actually produces."""
    total = len(samples)
    if total == 0:
        return set(), set()
    max_count = max(1, int(total * max_fraction))

    last_root_sample: dict[int, int] = {}
    root_count: dict[int, int] = {}
    total_count: dict[int, int] = {}
    for i, sample in enumerate(samples):
        root_ids_here = {
            t.task_id for t in (sample.tasks or ()) if not _is_asyncio_shutdown_task(t)
        }
        all_ids_here = {
            t.task_id
            for t in _iter_tasks_flat(sample.tasks or ())
            if not _is_asyncio_shutdown_task(t)
        }
        for tid in all_ids_here:
            total_count[tid] = total_count.get(tid, 0) + 1
        for tid in root_ids_here:
            last_root_sample[tid] = i
            root_count[tid] = root_count.get(tid, 0) + 1

    last_sample_index = total - 1
    root_only: set[int] = set()
    full: set[int] = set()
    for tid, count in root_count.items():
        if count > max_count:
            continue
        own_total = total_count[tid]
        if count < own_total:
            root_only.add(tid)
        elif last_root_sample[tid] == last_sample_index:
            full.add(tid)
    return root_only, full


_ASYNCIO_SHUTDOWN_TASKS = {
    "BaseEventLoop.shutdown_asyncgens",
    "BaseEventLoop.shutdown_default_executor",
}


def _is_asyncio_shutdown_task(t):
    """True for the transient tasks asyncio.run() schedules for its own
    post-main() cleanup (BaseEventLoop.shutdown_asyncgens, then
    BaseEventLoop.shutdown_default_executor) -- real, separate tasks
    unrelated to the pyramid's own intentional structure. A long enough
    sampling window's tail occasionally catches them for a sample or two
    once main() has already returned, which is not a fragmentation bug:
    exclude them so the task count isn't sensitive to exactly when
    sampling stops relative to interpreter shutdown."""
    return bool(t.frames) and t.frames[0].function in _ASYNCIO_SHUTDOWN_TASKS


@allpythons()
def test_asyncio_no_ambiguous_task_close_signal(py, save_mojo):
    """A task's dwell time while briefly caught EXECUTING mid-await is
    folded into its own accumulator (see task_tracker.h's pending_time)
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
    and BaseEventLoop.shutdown_default_executor -- real, separate tasks
    asyncio.run() schedules for its own cleanup once main() returns, which
    a long enough sampling window's tail can occasionally catch. Confirmed
    by decoding real CI captures where the count came back 30 (or 31): the
    extra ids were always these exact tasks, appearing only in the last
    handful of samples -- not fragmented pyramid tasks.

    _transient_root_ids filters out a different, also-confirmed
    capture artifact: a task's TASK_STACK identity landing on the wire
    before its TASK_WAITER edge (a torn read of task_awaited_by, or a
    genuine gap between a task being created and something awaiting it --
    see _py_asyncio__emit_task's first-sighting retry). This either
    self-corrects within a few samples (a real, otherwise-stable task that
    briefly flickered as root -- still counts toward the distinct total,
    just not the root count) or, rarely, never resolves because the
    capture's own last sample arrives first (indeterminate; excluded from
    both counts). See its own docstring for the exact, narrow shapes this
    excuses.

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
      id collision fixed by task_tracker.h's id -- would inflate one or both
      of these counts. The task count may fall short instead: a depth-0
      task, which returns right away, can start and finish between two
      samples on a slow runner, and never be seen at all.

    - The raw wire (see _iter_task_stack_blocks): AustinTask doesn't keep
      track of how many closing (empty-frame) blocks were seen, so it can't
      see the specific defect
      fixed here -- a task evicted with both suspended and executing dwell
      pending used to flush TWO closing blocks back to back. For every
      task id, an empty-frame block may only be the LAST block."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_pyramids.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    root_only_ids, full_exclusion_ids = _transient_root_ids(result.samples)

    all_tasks = [
        t
        for sample in result.samples
        for t in _iter_tasks_flat(sample.tasks or ())
        if not _is_asyncio_shutdown_task(t) and t.task_id not in full_exclusion_ids
    ]
    distinct_ids = {t.task_id for t in all_tasks}
    assert len(distinct_ids) <= 29, (
        f"Expected at most 29 distinct tasks (1 root + 4 depth-2 + 8 depth-1 "
        f"+ 16 depth-0); got {len(distinct_ids)}: {sorted(distinct_ids)} "
        f"(ignored as sampling-boundary artifacts: {sorted(full_exclusion_ids)}) "
        "-- extra ids mean a task got fragmented into several apparent ones"
    )

    root_ids = {
        t.task_id
        for sample in result.samples
        for t in (sample.tasks or ())
        if not _is_asyncio_shutdown_task(t)
        and t.task_id not in full_exclusion_ids
        and t.task_id not in root_only_ids
    }
    assert len(root_ids) == 1, (
        f"Expected a single root task throughout the whole capture; got "
        f"{len(root_ids)}: {sorted(root_ids)} "
        f"(ignored as sampling-boundary artifacts: "
        f"{sorted(root_only_ids | full_exclusion_ids)}) "
        "-- a spurious extra root means a mid-life transition was mistaken "
        "for that task dying"
    )

    last_was_closing = {}
    violations = []
    for block in _iter_task_stack_blocks(_raw_mojo(result)):
        if last_was_closing.get(block.task_id):
            violations.append(block.task_id)
        last_was_closing[block.task_id] = not block.scopes

    assert violations == [], (
        "Expected an empty-frame (closing) MOJO_TASK_STACK to only ever be "
        f"the LAST block for its task id; task id(s) {sorted(set(violations))} "
        "had another block follow a closing one, meaning a single task's "
        "end-of-life dwell was split across more than one closing signal"
    )


@allpythons()
def test_asyncio_running_time_from_samples(py, save_mojo):
    """The time a task spends running is the time of the samples whose
    thread reports it as running (MOJO_TASK_RUNNING).

    Each worker in target_asyncio_cpu.py alternates CPU bursts with an
    immediate reschedule, so the event loop thread is running one of them
    nearly all the time: their running time adds up to most of the
    thread's. A sample may only refer to a task already reported earlier on
    the wire, so that consumers know its position."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_cpu.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    raw = _raw_mojo(result)

    reported = set()
    unknown = set()
    for r in _iter_mojo_records(raw):
        if isinstance(r, TaskStackBlock):
            reported.add(r.task_id)
        elif r.running is not None and r.running not in reported:
            unknown.add(r.running)
    assert not unknown, f"Samples refer to unreported running tasks {unknown}"

    names = {b.task_id: b.name for b in _iter_task_stack_blocks(raw) if b.name}
    running = _running_time_by_task(raw)
    worker_time = sum(
        t for i, t in running.items() if (names.get(i) or "").startswith("worker-")
    )
    thread_time = sum(_iter_sample_times(raw))

    assert worker_time > 0.5 * thread_time, (
        f"Expected the workers to be running for most of the thread's "
        f"{thread_time}us; got {worker_time}us"
    )


@allpythons()
def test_asyncio_cpu_mode_idle_thread_keeps_tasks(py, save_mojo):
    """In CPU mode (-c), a task keeps a single wire identity while its event
    loop thread goes idle and is skipped.

    _py_proc__sample_threads skips idle threads in CPU mode before their
    task list is scanned. Their tasks then looked gone to
    py_asyncio__scan_tasks_end, which evicted them with a closing flush,
    and the next non-idle sample tracked them again from scratch, under a
    new task id. target_asyncio_idle.py's loop goes idle
    between every burst, so each worker used to show up under several ids.
    Skipped threads now keep their tasks alive instead
    (py_asyncio__keep_task_list_alive)."""
    result = austin("-c", "-i", "1000", *python(py), target("target_asyncio_idle.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    ids_by_name = {}
    for block in _iter_task_stack_blocks(_raw_mojo(result)):
        if block.name and block.name.startswith("worker-"):
            ids_by_name.setdefault(block.name, set()).add(block.task_id)

    assert ids_by_name, "Expected task reports for the worker tasks"

    fragmented = {name: ids for name, ids in ids_by_name.items() if len(ids) > 1}
    assert not fragmented, (
        f"Expected one task id per worker; got {fragmented} -- tasks of a "
        "thread skipped as idle are being evicted and re-tracked"
    )


@allpythons()
def test_asyncio_task_address_reuse_gets_new_identity(py, save_mojo):
    """A task found at the address of a task Austin was already tracking
    gets its own wire identity, rather than inheriting the old one.

    Eviction runs at the end of a tick, so when a task completes and the
    allocator hands its memory to a new task within the same tick,
    _py_asyncio__emit_task gets the old entry back for the new task. It
    used to reset the entry but keep its id, so the new task was reported
    under the old task's id (and the old task's pending time was dropped
    without a closing flush). The entry is now flushed and renewed with a
    fresh id (task_tracker__renew).

    Ids also used to be derived from the task's address, XORed with a
    counter. Allocators hand out nearby addresses while the counter takes
    small values, so different tasks often ended up with the same id --
    on free-threaded builds especially. Ids are now just the counter.

    Telling the two tasks apart is the hard part: the allocator often
    reuses the Task and its coroutine together, so a changed coroutine
    address alone misses many replacements. A changed name catches those
    as long as the tasks have distinct names, which is what this test can
    rely on. Two unnamed tasks replacing each other within one sample are
    indistinguishable by pointers alone and aren't covered.

    A task's name str is just as short-lived, and its memory is just as
    routinely reused by the next task's name. Names used to be cached by
    the str's address alone, so a new task could be reported with the name
    of an earlier one (see _py_asyncio__resolve_task_name). Redefining a
    name key on reuse is only safe because nothing refers to a name once
    its task is gone: closing flushes carry no name.

    target_asyncio_reuse.py runs many tasks back to back, each with its own
    name, so address reuse is routine. A task id showing up with more than
    one name means two tasks shared it; a name showing up with more than
    one id means a stale name was reported."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_reuse.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    names_by_id = {}
    ids_by_name = {}
    for block in _iter_task_stack_blocks(_raw_mojo(result)):
        if block.name is not None:
            names_by_id.setdefault(block.task_id, set()).add(block.name)
            ids_by_name.setdefault(block.name, set()).add(block.task_id)

    assert names_by_id, "Expected named task reports"

    shared = {i: names for i, names in names_by_id.items() if len(names) > 1}
    assert not shared, (
        f"Expected one task per id; {len(shared)} id(s) were shared by "
        f"different tasks, e.g. {next(iter(shared.values()))}"
    )

    stale = {name: ids for name, ids in ids_by_name.items() if len(ids) > 1}
    assert not stale, (
        f"Expected one id per task name; {len(stale)} name(s) were reported "
        f"for different tasks, e.g. {sorted(stale)[:5]}"
    )


@allpythons()
def test_asyncio_cpu_mode_credits_running_task_only(py, save_mojo):
    """In CPU mode (-c), a task is only credited the CPU time of the samples
    it is running at.

    CPU mode skips idle threads, but a task suspended on a busy thread used
    to accrue that thread's CPU time too -- most likely another task's,
    since that is what the event loop thread is busy with. Each task's
    total then approached the thread's own, and main(), which only awaits
    gather(), looked like it used the CPU all along. A suspended task uses
    no CPU: a task's CPU time is that of the samples it is running at.

    Each sample goes to at most one task, the running one, so the tasks'
    total can't exceed the thread's."""
    result = austin("-c", "-i", "1000", *python(py), target("target_asyncio_cpu.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    raw = _raw_mojo(result)

    thread_time = sum(_iter_sample_times(raw))
    blocks = list(_iter_task_stack_blocks(raw))
    main_ids = {b.task_id for b in blocks if b.scopes and b.scopes[0] == "main"}
    cpu_by_id = _running_time_by_task(raw)

    assert thread_time > 0 and cpu_by_id, "Expected thread and task data"
    assert main_ids, "Expected reports for the main task"

    task_time = sum(cpu_by_id.values())
    assert task_time <= thread_time, (
        f"Tasks were credited {task_time}us of CPU time, more than their "
        f"thread's {thread_time}us"
    )

    main_time = sum(cpu_by_id.get(i, 0) for i in main_ids)
    assert main_time < 0.05 * thread_time, (
        f"main() was credited {main_time}us of {thread_time}us CPU time, "
        "although it only awaits the workers"
    )


@allpythons()
def test_asyncio_full_mode_running_task_cpu_time(py, save_mojo):
    """In full mode (-f), a task's CPU time can be told apart from its
    running wall time, as for threads: the samples it is running at carry
    the thread's idle flag.

    target_asyncio_blocked.py's task runs a real time.sleep(), so it is
    running, but its thread is off-CPU: most of that running time is on
    idle samples."""
    result = austin(
        "-f", "-i", "1000", *python(py), target("target_asyncio_blocked.py")
    )
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    raw = _raw_mojo(result)
    wall = _running_time_by_task(raw)
    cpu = _running_time_by_task(raw, cpu_only=True)

    task_id, task_wall = max(wall.items(), key=lambda kv: kv[1], default=(None, 0))
    assert task_wall > 200_000, f"Expected a long running stretch; got {task_wall}us"

    task_cpu = cpu.get(task_id, 0)
    assert task_cpu < 0.5 * task_wall, (
        f"Expected most of the blocked task's {task_wall}us running time to be "
        f"off-CPU; got {task_cpu}us of CPU time"
    )


@allpythons()
def test_asyncio_eager_task_running_frames(py, save_mojo):
    """A task started eagerly is the one credited with the code it runs,
    not the task that created it.

    With the eager task factory, create_task runs the new task right away,
    inside the creator's create_task call, so both coroutines are executing
    at once. Austin used to treat every executing coroutine as running, and
    the creator could claim the new task's frames when split off its
    thread's stack first. The running task is now the thread's current task
    as asyncio itself tracks it, which is the inner one."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_eager.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    blocks = list(_iter_task_stack_blocks(_raw_mojo(result)))
    name_by_id = {b.task_id: b.name for b in blocks if b.name is not None}

    burst_owners = {
        name_by_id.get(b.task_id) for b in blocks if "cpu_burst" in b.scopes
    }
    assert burst_owners, "Expected cpu_burst in some task's frames"

    wrong = {n for n in burst_owners if n is None or not n.startswith("child-")}
    assert not wrong, (
        f"cpu_burst was reported in the frames of {sorted(map(str, wrong))}, "
        "rather than only of the child tasks running it"
    )


@allpythons()
def test_asyncio_task_list_walk_cut_short(py, save_mojo):
    """A task keeps its id when a walk of its task list is cut short before
    reaching it.

    Austin reads the task list while the target keeps running, so a node
    can be unlinked (its task completing) just as it is read. The walk then
    finds a NULL next pointer and stops, without touching the tasks after
    it. py_asyncio__scan_tasks_end used to take those for gone, evicting
    them with a closing flush and tracking them again under a new id. It
    now skips eviction for a sweep with a list walked only in part.

    target_asyncio_churn.py creates a long-lived task after hundreds of
    short ones that complete one by one throughout the run, so that many
    walks are cut short ahead of it."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_churn.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    keeper_ids = {
        block.task_id
        for block in _iter_task_stack_blocks(_raw_mojo(result))
        if block.name == "keeper"
    }

    assert len(keeper_ids) == 1, (
        f"Expected the keeper task under a single id; got {sorted(keeper_ids)} "
        "-- tasks beyond a cut-short list walk are being evicted"
    )


@allpythons()
def test_asyncio_every_task_closed(py, save_mojo):
    """Every reported task that goes away is closed.

    The closing report is the only signal readers get that a task is gone,
    but it used to be skipped for a task with no time pending, which was
    most of them (e.g. any task finishing while running), so readers' task
    trees kept every task ever seen. target_asyncio_reuse.py runs hundreds of
    tasks one after the other, so only a couple are alive at any time."""
    result = austin("-i", "1000", *python(py), target("target_asyncio_reuse.py"))
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    blocks = list(_iter_task_stack_blocks(_raw_mojo(result)))
    reported = {b.task_id for b in blocks if b.scopes}
    closed = {b.task_id for b in blocks if not b.scopes}
    assert len(reported) > 10, "Expected many task reports"

    # Tasks still alive when sampling stops are never closed.
    assert (
        len(reported - closed) <= 3
    ), f"{len(reported - closed)} of {len(reported)} reported tasks were never closed"

    last = [s for s in result.samples if s.tasks][-1]
    live = {t.task_id for t in _iter_tasks_flat(last.tasks)}
    assert len(live) <= 3, f"Expected only the live tasks; got {len(live)}"


@allpythons()
def test_asyncio_task_loses_waiter(py, save_mojo):
    """A task that loses its last waiter is reported as such, even while
    that waiter is still alive.

    Austin used to emit nothing when a task's waiter set became empty,
    leaving consumers to infer it from the waiter going away. In
    target_asyncio_waiter_release.py, watcher stops awaiting worker when
    asyncio.wait() times out, then keeps running: worker must then show up
    as a root of its own, not under watcher. Austin now emits a waiter of 0
    (no task) for it."""
    result = austin(
        "-i", "1000", *python(py), target("target_asyncio_waiter_release.py")
    )
    save_mojo(result.stdout)
    assert result.returncode == 0, result.stderr or result.stdout

    def by_name(sample):
        return {t.name: t for t in _iter_tasks_flat(sample.tasks)}

    awaited = released = False
    for sample in result.samples:
        tasks = by_name(sample)
        if "watcher" not in tasks or "worker" not in tasks:
            continue
        roots = {t.name for t in sample.tasks}
        under_watcher = any(c.name == "worker" for c in tasks["watcher"].awaiting)
        awaited |= under_watcher
        released |= "worker" in roots and not under_watcher

    assert awaited, "Expected worker under watcher while being waited on"
    assert released, (
        "Expected worker as a root once watcher stopped waiting on it, while "
        "watcher was still alive"
    )


@allpythons()
@pytest.mark.parametrize("mode", [[], ["-c"]], ids=["wall", "cpu"])
def test_asyncio_running_task_consistent(py, mode, save_mojo):
    """A sample's running task is a live task readers know of, and its frames
    are not also in the thread's own stack.

    The target keeps running while it is sampled, so the running task read
    with a thread's stack can be gone by the time its task list is walked,
    and the stack can show another task than the one it reads as running.
    Austin used to report the task as running regardless: a task already
    closed, or one whose frames the thread's stack still had, so that
    consumers would count them twice. It now only does so when the task's
    frames were split off the sample's stack, or the sample repeats one where
    they were. Sampling target_asyncio_cpu.py as fast as possible maximises
    the chances to hit these races."""
    result = austin("-i", "100", *mode, *python(py), target("target_asyncio_cpu.py"))
    save_mojo(result.stdout, suffix=mode[0][1:] if mode else "wall")
    assert result.returncode == 0, result.stderr or result.stdout

    latest = {}
    unknown = doubled = running = 0
    for sample in result.samples:
        for report in sample.task_reports:
            if report.frames:
                latest[report.task_id] = report.frames
            else:
                latest.pop(report.task_id, None)

        if sample.running_task is None:
            continue
        running += 1

        frames = latest.get(sample.running_task)
        if frames is None:
            unknown += 1
        elif sample.frames and any(
            f.function == frames[0].function for f in sample.frames
        ):
            doubled += 1

    assert running, "Expected samples with a running task"
    assert unknown == 0, f"{unknown} samples refer to a running task readers don't know"
    assert doubled == 0, (
        f"{doubled} samples have their running task's frames in the thread's "
        "own stack too"
    )
