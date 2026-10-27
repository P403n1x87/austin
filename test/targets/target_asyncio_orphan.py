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

# A thread starts an event loop, creates a still-pending task, then exits
# without finishing it -- while a module-level reference to the task keeps it
# alive. CPython moves such a task to the interpreter-wide fallback list when
# the owning thread's PyThreadState is torn down, so this exercises the
# "Orphaned task tree" fallback rendering rather than the normal per-thread one.

import asyncio
import threading
import time

captured_task = None


async def stuck_worker():
    await asyncio.sleep(1000)


def run_and_orphan():
    global captured_task
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    captured_task = loop.create_task(stuck_worker())
    loop.run_until_complete(asyncio.sleep(0.2))
    loop.close()


if __name__ == "__main__":
    t = threading.Thread(target=run_and_orphan, name="Orphan-Thread")
    t.start()
    t.join()

    time.sleep(10)
