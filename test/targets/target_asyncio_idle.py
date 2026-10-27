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

# Like target_asyncio_cpu.py, but each short on-CPU burst is followed by a
# real suspension (asyncio.sleep with a non-zero delay) rather than an
# immediate reschedule, so the event loop thread goes idle (blocked in its
# selector) between bursts. CPU-time sampling (-c) skips idle threads, so
# this exercises the tasks of a thread that is alternately sampled and
# skipped.

import asyncio


def cpu_burst(n):
    total = 0
    for i in range(n):
        total += (i * i) % 97
    return total


async def worker(n):
    for _ in range(20):
        cpu_burst(200_000)
        await asyncio.sleep(0.05)
    return n


async def main():
    tasks = [asyncio.create_task(worker(i), name=f"worker-{i}") for i in range(2)]
    await asyncio.gather(*tasks)


if __name__ == "__main__":
    asyncio.run(main())
