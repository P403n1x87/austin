import copy
from collections import deque
from threading import RLock

ATOM_LOCK = RLock()


def atomic(f):
    """Decorator to turn a function into an atomic operation."""

    def atomic_wrapper(*args, **kwargs):
        with ATOM_LOCK:
            result = f(*args, **kwargs)

        return result

    return atomic_wrapper


class SampledFrame:
    """
    Sampled frame class that recursivly populates itself.

    Despite the name, the base frame can be considered as the whole sampled
    frame stack.
    """

    def __init__(self, data, duration, height):
        self.function = data[0]
        self.total_time = duration
        self.line_number = data[1][1:]
        self.height = height
        if data[2:]:
            self.children = [SampledFrame(data[2:], duration, height + 1)]
            self.own_time = 0
        else:
            self.children = []
            self.own_time = duration

    def to_dict(self):
        return {
            "function": self.function,
            "line_number": self.line_number,
            "tot_time": self.total_time,
            "own_time": self.own_time
        }

    def __eq__(self, other):
        """Two frames are the same if they represent the same function."""
        return self.function == other.function if other else False


# -----------------------------------------------------------------------------


def parse_line(line):
    """
    Split a collapsed frame stack sample into its components.

    These are: the thread ID, the list of frames and the duration of the
    samplingself.
    """
    try:
        thread, rest = line.decode().strip('\n').split(';', maxsplit=1)
        frames, duration = rest.rsplit(maxsplit=1)
        frames = frames.split(";")
    except ValueError:
        # Probably an "empty" thread
        thread, duration = line.decode().rsplit(maxsplit=1)
        frames = []

    return thread, frames, int(duration)


class Stats:
    """
    Statistics class. Each instance will bear statistics for each sampling run.

    To update the statistics, simply pass every single line returned by austin
    to an instance of this class via the `add_thread_sample` method.

    To retrieve the current stacks along with their statistics, call the
    `get_current_stacks` methods.
    """

    def __init__(self):
        self.threads = {}
        self.current_thread = None
        self.current_stack = None
        self.current_threads = {}
        self.samples = 0

    def _update_frame(self, frame_stack, sample_stack):
        if sample_stack is None:
            return

        frame_stack.total_time += sample_stack.total_time
        frame_stack.own_time += sample_stack.own_time

        if sample_stack.children:
            sample_child = sample_stack.children[0]

            i = 0
            for child in frame_stack.children:
                if child == sample_child:
                    self._update_frame(child, sample_child)
                    break
                i += 1
            else:
                frame_stack.children.append(sample_child)

            self.current_stack.appendleft(i)

    @atomic
    def add_thread_sample(self, collapsed_sample):
        thread, frames, duration = parse_line(collapsed_sample)

        sample_stack = SampledFrame(frames, duration, 1) if frames else None

        self.current_stack = deque()

        i = 0
        if thread in self.threads:
            for frame_stack in self.threads[thread]:
                if frame_stack == sample_stack:
                    self._update_frame(frame_stack, sample_stack)
                    break
                i += 1
            else:
                self.threads[thread].append(sample_stack)

        else:
            self.threads[thread] = [sample_stack]

        self.current_stack.appendleft(i)

        self.current_threads[thread] = self.current_stack

        self.samples += 1

    @atomic
    def get_current_stacks(self, reset_after=False):
        stacks = {}
        for thread in self.current_threads:
            frame_list = self.threads[thread]
            stack = []
            for i in self.current_threads[thread]:
                if frame_list[i] is None:
                    continue
                stack.append(frame_list[i].to_dict())
                frame_list = frame_list[i].children
            stacks[thread] = stack

        if reset_after:
            self.current_threads = {}

        return stacks

    @atomic
    def get_current_threads(self):
        return sorted(self.current_threads.keys())

    @atomic
    def get_thread_stack(self, thread):
        if thread not in self.current_threads:
            return None

        retval = copy.deepcopy(self.threads[thread])

        frame_list = retval
        for i in self.current_threads[thread]:
            if frame_list[i]:
                frame_list[i].is_active = True
                frame_list = frame_list[i].children
            else:
                break

        return retval
