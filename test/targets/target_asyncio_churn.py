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

# A long-lived task, created after many short ones that then complete one by
# one throughout the run. Tasks are linked in creation order, so each
# completion unlinks a list node ahead of the long-lived one -- possibly the
# very node austin is reading as it walks the list, which leaves it with a
# NULL next pointer and the long-lived task out of reach for that sample.

import asyncio


def cpu_burst(n):
    total = 0
    for i in range(n):
        total += (i * i) % 97
    return total


async def short(n):
    await asyncio.sleep(0.003 * n)


async def keeper(done):
    while not done.is_set():
        cpu_burst(20_000)
        await asyncio.sleep(0)


async def main():
    tasks = [asyncio.create_task(short(i)) for i in range(800)]
    done = asyncio.Event()
    keeper_task = asyncio.create_task(keeper(done), name="keeper")
    await asyncio.gather(*tasks)
    done.set()
    await keeper_task


if __name__ == "__main__":
    asyncio.run(main())
