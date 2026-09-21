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

# A real, synchronous time.sleep() call freezes the calling frame's
# code+lasti for its whole duration -- unlike target_asyncio_cpu.py's tight
# arithmetic loop, which merely makes repeat samples *likely*, this makes
# them certain, on any machine at any speed. That determinism is the point:
# it reliably exercises the EXECUTING-task-on-a-repeat-sample path (see
# task_tracker.h's executing_time doc comment) without depending on
# statistical luck, so a test built on this target can't flake.
#
# blocked_call and marker_call are two distinct functions so that the
# transition between them (a change in the thread's top frame identity)
# forces a normal split to run and flush everything accumulated during
# blocked_call's long repeat-sample stretch -- while austin is still
# actively sampling, rather than relying on the process exiting at just the
# right moment.

import asyncio
import time


def blocked_call(seconds):
    time.sleep(seconds)


def marker_call(seconds):
    time.sleep(seconds)


async def blocked_worker():
    blocked_call(0.5)
    marker_call(0.1)
    await asyncio.sleep(0)


async def main():
    await asyncio.gather(asyncio.create_task(blocked_worker(), name="blocker"))


if __name__ == "__main__":
    asyncio.run(main())
