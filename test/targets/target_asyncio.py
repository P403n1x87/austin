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

# A single event loop driving two sibling tasks, each suspended a few levels
# deep in a nested await chain -- exercises the coroutine-chain unwind and the
# task tree's parent/child rendering.

import asyncio


async def leaf_work(n):
    total = 0
    for _ in range(6):
        await asyncio.sleep(0.3)
        total += n
    return total


async def level3(n):
    return await leaf_work(n)


async def level2(n):
    return await level3(n)


async def level1(n):
    return await level2(n)


async def fetch_data(n):
    await asyncio.sleep(0.1)
    return await level1(n)


async def process_item(n):
    data = await fetch_data(n)
    await asyncio.sleep(0.1)
    return data * 2


async def pipeline_stage(name, n):
    result = await process_item(n)
    return f"{name}:{result}"


async def worker(n):
    return await pipeline_stage(f"worker-{n}", n)


async def main():
    tasks = [asyncio.create_task(worker(i), name=f"worker-{i}") for i in range(2)]
    await asyncio.gather(*tasks)


if __name__ == "__main__":
    asyncio.run(main())
