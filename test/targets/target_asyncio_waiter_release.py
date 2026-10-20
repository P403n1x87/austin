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

# A task that stops awaiting another one while both carry on: watcher waits
# on worker with asyncio.wait() and a timeout, which drops it from worker's
# waiters once it times out, then keeps running for a while.

import asyncio


def cpu_burst(n):
    total = 0
    for i in range(n):
        total += (i * i) % 97
    return total


async def worker(done):
    while not done.is_set():
        cpu_burst(20_000)
        await asyncio.sleep(0.005)


async def watcher(worker_task):
    await asyncio.wait({worker_task}, timeout=0.3)
    await asyncio.sleep(0.5)


async def main():
    done = asyncio.Event()
    worker_task = asyncio.create_task(worker(done), name="worker")
    await asyncio.create_task(watcher(worker_task), name="watcher")
    done.set()
    await worker_task


if __name__ == "__main__":
    asyncio.run(main())
