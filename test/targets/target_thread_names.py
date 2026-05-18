#!/usr/bin/env python3

# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2025 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

# Thread-name transition test target.
#
# Phase 1: single-threaded, threading module not yet imported.
#   Austin should name the main thread "MainThread" via the single-thread path.
#
# Phase 2: threading is imported and two named threads are created.
#   Austin should resolve "Worker1" and "Worker2" via threading._active.
from time import sleep


def wait():
    sleep(0.5)


if __name__ == "__main__":
    # Phase 1: no threading module — single thread
    wait()

    # Phase 2: import threading and start named workers
    import threading

    workers = [
        threading.Thread(target=wait, name="Worker1"),
        threading.Thread(target=wait, name="Worker2"),
    ]
    for w in workers:
        w.start()

    wait()

    for w in workers:
        w.join()
