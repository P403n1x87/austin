# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2019 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

# source: https://lobste.rs/s/qairy5/austin_python_frame_stack_sampler_for

import time
import multiprocessing


def fact(n):
    f = 1
    for i in range(1, n + 1):
        f *= i
    return f


def do(N):
    n = 1
    for _ in range(N):
        fact(n)
        n += 1


if __name__ == "__main__":
    processes = []
    for _ in range(2):
        process = multiprocessing.Process(target=do, args=(2000,))
        process.start()
        processes.append(process)

    for process in processes:
        process.join(timeout=5)
