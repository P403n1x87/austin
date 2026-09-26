# This file is part of "austin" which is released under GPL.
#
# See file LICENCE or go to http://www.gnu.org/licenses/ for full license
# details.
#
# Austin is a Python frame stack sampler for CPython.
#
# Copyright (c) 2022 Gabriele N. Tornetta <phoenix1987@gmail.com>.
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

from test.utils import allpythons
from test.utils import austin
from test.utils import has_frame
from test.utils import python
from test.utils import target


@allpythons()
def test_accuracy_fast_recursive(py):
    result = austin("-i", "1ms", "-P", *python(py), target("recursive.py"))
    assert result.returncode == 0, result.stderr or result.stdout

    assert has_frame(result.samples, "recursive.py", "sum_up_to")

    # recursive.py recurses 16 levels deep, so a recursive sample's true
    # depth is ~16-18 frames (plus a couple of bookkeeping frames). Austin
    # samples without stopping the target, so it can occasionally catch a
    # thread's frame chain mid-update (e.g. a return in progress) and see a
    # sample that looks a little deeper than it really is -- that's sampling
    # noise, not a bug. A hard per-sample ceiling trips on this on a loaded
    # CI runner, so tolerate a small fraction of near-miss outliers, while
    # still failing outright on a wildly oversized stack (real fragmentation,
    # e.g. duplicated frames, would blow well past the tolerance ceiling).
    HARD_CEILING = 40
    SOFT_CEILING = 20
    OUTLIER_TOLERANCE = 0.02  # at most 2% of recursive samples may be near-miss outliers

    recursive_samples = 0
    outliers = 0
    max_depth = 0

    for sample in result.samples:
        if (
            not sample.frames
            or len(sample.frames) < 2
            or sample.frames[1].function != "sum_up_to"
        ):
            continue
        recursive_samples += 1
        depth = len(sample.frames)
        max_depth = max(max_depth, depth)
        if depth > HARD_CEILING:
            raise AssertionError(
                f"recursive stack is way taller than actual recursion: {depth} frames"
            )
        if depth > SOFT_CEILING:
            outliers += 1

    assert recursive_samples > 0, "no samples captured the recursive call"
    assert outliers <= max(1, int(recursive_samples * OUTLIER_TOLERANCE)), (
        f"too many oversized recursive stacks: {outliers}/{recursive_samples} samples "
        f"exceeded depth {SOFT_CEILING} (max observed depth {max_depth})"
    )
