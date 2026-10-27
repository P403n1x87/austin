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

# Two OS threads, each driving its own independent event loop with its own
# tasks. Exercises task-to-loop/thread attribution: each thread's task tree
# must show only its own tasks, never another thread's.

import asyncio
import threading


async def worker(n):
    for _ in range(6):
        await asyncio.sleep(0.3)
    return n


def run_loop(name, n):
    async def main():
        tasks = [asyncio.create_task(worker(i), name=f"{name}-worker-{i}") for i in range(n)]
        await asyncio.gather(*tasks)

    asyncio.run(main())


if __name__ == "__main__":
    threads = [
        threading.Thread(target=run_loop, args=(f"loop{i}", 2), name=f"Loop{i}-Thread")
        for i in range(2)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
