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

# A single loop task that alternates idling with directly awaiting a freshly
# created subtask, giving a continuous baseline with periodic bumps rising
# off it. Unlike target_asyncio_topologies.py (which exercises the various
# SHAPES a task-trace flame chart needs to lay out), this is meant to
# exercise the model's TIMING reconstruction against a realistic, periodic
# task lifecycle.
#
# Each bump is itself a small recursive pyramid: pyramid_task SERIALIZES a
# few sub-awaits, each preceded by a wait, one level deeper each time --
# unlike a flat immediate-delegate chain (task A awaits task B awaits task
# C, nothing else happening at any level), which gives every level the
# EXACT same observed time range and renders as one solid aligned block, a
# genuinely staggered/serialized chain gives each nested level a narrower,
# time-shifted window than its parent -- an actual stepped pyramid, not a
# tower. Every task at every level and every round shares the SAME name, to
# check that different tasks with the same name still get distinct,
# unmerged spans (keyed by task id, not by name).

import asyncio

IDLE = 0.15
ROUNDS = 4
SUB_ROUNDS = 2
PYRAMID_DEPTH = 2
SUB_WAIT = 0.08
LEAF_WORK = 0.1
SUBTASK_NAME = "subtask"


async def leaf_work():
    await asyncio.sleep(LEAF_WORK)


async def pyramid_task(depth):
    """Serializes SUB_ROUNDS child tasks, each preceded by a wait, `depth`
    levels deep -- so one call's children are staggered in time relative to
    each other (and to their own children), not all aligned to the exact
    same window."""
    if depth <= 0:
        return await leaf_work()
    for _ in range(SUB_ROUNDS):
        await asyncio.sleep(SUB_WAIT)
        child = asyncio.create_task(pyramid_task(depth - 1), name=SUBTASK_NAME)
        await child


async def pyramid_loop():
    for _ in range(ROUNDS):
        await asyncio.sleep(IDLE)  # baseline: idling, no subtask active
        # A brand new Task every round, but always the SAME name -- the
        # model must still tell them apart by task id, not by name.
        subtask = asyncio.create_task(pyramid_task(PYRAMID_DEPTH), name=SUBTASK_NAME)
        await subtask  # direct await -> registers a real waiter edge


async def main():
    await pyramid_loop()


if __name__ == "__main__":
    asyncio.run(main())
