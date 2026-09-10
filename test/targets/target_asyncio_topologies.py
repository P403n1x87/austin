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

# Deliberately exercises every task-trace topology the vscode extension's
# flame-chart visualization needs to lay out and color correctly, all on a
# single event loop thread:
#
#   - CHAIN_ROOTS  : several independent root tasks, each a compact stack of
#                    single-child tasks (tests per-root spacing/coloring and
#                    the "single child -> thin gap, no spine" rule repeated
#                    at the thread's own root level).
#   - FAN_ROOT     : one root task awaiting several children, each itself a
#                    single-child chain (tests the multi-child spine at the
#                    top level, with compact single-child stacks hanging
#                    off each branch).
#   - NESTED_FAN   : one root task awaiting several children that THEMSELVES
#                    each await several children (tests spines at two
#                    consecutive levels, and per-level coloring three levels
#                    deep).
#   - OVERLAP_ROOT : several genuinely concurrent siblings with staggered,
#                    overlapping start times (tests the "earliest-starting
#                    child goes deepest" ordering that keeps a peg from
#                    crossing through a shallower sibling's bar).
#
# All the intermediate awaiting is done with asyncio.gather(), which -- per
# CPython's own task_awaited_by tracking -- still registers each gathered
# task's waiter, so the nesting below shows up in the task tree exactly as
# named (gather does not flatten it to a set of unrelated root tasks).

import asyncio

CHAIN_COUNT = 3
CHAIN_DEPTH = 3
FAN_WIDTH = 3
FAN_CHAIN_DEPTH = 2
NESTED_BRANCHES = 2
NESTED_SUB_BRANCHES = 2
OVERLAP_COUNT = 4
STEP = 0.3


async def leaf(name):
    await asyncio.sleep(STEP)
    return name


async def single_chain(name, depth):
    """A stack of single-child tasks, `depth` levels deep."""
    if depth <= 0:
        return await leaf(name)
    child = asyncio.create_task(single_chain(f"{name}.child", depth - 1), name=f"{name}.child")
    return await child


async def fan_of_chains(name, width, depth):
    """One task awaiting `width` children, each a single_chain of its own."""
    children = [
        asyncio.create_task(single_chain(f"{name}.branch{i}", depth), name=f"{name}.branch{i}")
        for i in range(width)
    ]
    await asyncio.gather(*children)


async def fan_root():
    await fan_of_chains("fan-root", FAN_WIDTH, FAN_CHAIN_DEPTH)


async def nested_fan():
    async def branch(name):
        await fan_of_chains(name, NESTED_SUB_BRANCHES, 1)

    branches = [
        asyncio.create_task(branch(f"nested-branch{i}"), name=f"nested-branch{i}")
        for i in range(NESTED_BRANCHES)
    ]
    await asyncio.gather(*branches)


async def overlap_worker(name, initial_delay, work_time):
    await asyncio.sleep(initial_delay)
    await asyncio.sleep(work_time)
    return name


async def overlap_root():
    workers = [
        asyncio.create_task(overlap_worker(f"overlap-{i}", i * STEP / 2, STEP * 2), name=f"overlap-{i}")
        for i in range(OVERLAP_COUNT)
    ]
    await asyncio.gather(*workers)


async def main():
    # CHAIN_ROOTS: literal thread-level root tasks -- nothing awaits them
    # directly (see the polling loop below), each a compact single-child
    # chain of its own. The other three scenarios are each wrapped in their
    # own dedicated task too, so every pattern coexists as an independent
    # root sibling on this one thread.
    chain_root_tasks = [
        asyncio.create_task(single_chain(f"chain-root-{i}", CHAIN_DEPTH), name=f"chain-root-{i}")
        for i in range(CHAIN_COUNT)
    ]
    other_tasks = [
        asyncio.create_task(fan_root(), name="fan-root-task"),
        asyncio.create_task(nested_fan(), name="nested-fan-task"),
        asyncio.create_task(overlap_root(), name="overlap-root-task"),
    ]

    # Deliberately polled rather than awaited/gathered directly: awaiting
    # (even via gather) registers a waiter edge in CPython's own
    # task_awaited_by tracking and would nest every one of these under
    # main's own task -- collapsing the very "a thread can have several
    # independent root tasks" case CHAIN_ROOTS exists to exercise.
    all_tasks = [*chain_root_tasks, *other_tasks]
    while not all(t.done() for t in all_tasks):
        await asyncio.sleep(STEP)


if __name__ == "__main__":
    asyncio.run(main())
