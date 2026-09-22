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

# Two independent tasks both directly `await` the SAME third task at once.
# CPython's asyncio task introspection represents a single task's
# task_awaited_by as a plain reference when it has one waiter, but switches
# to a set once a second, distinct task starts awaiting it too -- this is
# the only way to exercise Austin's waiter-SET walk (py_set.h /
# _py_asyncio__emit_waiter_set), as opposed to the single-waiter fast path
# every other asyncio test target exercises.

import asyncio


async def shared_worker():
    await asyncio.sleep(2)


async def fan_in_waiter(shared_task):
    await shared_task


async def main():
    shared_task = asyncio.create_task(shared_worker(), name="shared_worker")
    # Give shared_task a chance to actually start running (and suspend at
    # its own await) before both waiters start awaiting it, so there's a
    # real window where task_awaited_by is genuinely a 2-element set.
    await asyncio.sleep(0.1)
    fan_in_0 = asyncio.create_task(fan_in_waiter(shared_task), name="fan-in-0")
    fan_in_1 = asyncio.create_task(fan_in_waiter(shared_task), name="fan-in-1")
    await asyncio.gather(fan_in_0, fan_in_1)


if __name__ == "__main__":
    asyncio.run(main())
