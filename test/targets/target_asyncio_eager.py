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

# With the eager task factory, create_task starts running the new task right
# away, inside the creating task's create_task call, until its first
# suspension. Both coroutines are executing at once there, but only the inner
# one is the thread's current task -- the one whose CPU time it is.

import asyncio


def cpu_burst(n):
    total = 0
    for i in range(n):
        total += (i * i) % 97
    return total


async def child():
    cpu_burst(200_000)
    await asyncio.sleep(0)


async def outer():
    for i in range(20):
        await asyncio.create_task(child(), name=f"child-{i}")


async def main():
    asyncio.get_running_loop().set_task_factory(asyncio.eager_task_factory)
    await asyncio.create_task(outer(), name="outer")


if __name__ == "__main__":
    asyncio.run(main())
