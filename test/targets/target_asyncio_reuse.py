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

# Runs many tasks one after the other, each long enough to be sampled. A
# finished task is released (dropped, then one loop iteration to let asyncio
# drop its own references) right before the next one is created, so the
# allocator hands the next task the very same memory, within microseconds --
# the case where austin finds a different task at an address it saw at the
# previous sample and is still tracking.

import asyncio


def cpu_burst(n):
    total = 0
    for i in range(n):
        total += (i * i) % 97
    return total


async def job():
    cpu_burst(20_000)


async def main():
    for i in range(300):
        task = asyncio.create_task(job(), name=f"job-{i}")
        await task
        del task
        await asyncio.sleep(0)


if __name__ == "__main__":
    asyncio.run(main())
