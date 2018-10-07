from psutil import Process
from threading import Thread, Event

import subprocess


class Austin(Thread):
    """
    Wrapper around the austin executable.

    It spawns a new process and starts a new Python thread to read the output
    line by line and update the statistics.
    """

    def __init__(self, stats, args):
        super().__init__()

        self.stats = stats
        self.args = args
        self.quit_event = Event()
        self.start_event = Event()
        self.austin = None
        self.pid = int(args[1]) if args[0] == '-p' else -1
        self.cmd_line = "<unknown>"

    def get_pid(self):
        return self.pid

    def get_cmd_line(self):
        return " ".join(self.cmd_line)

    def run(self):
        self.austin = subprocess.Popen(
            ["austin", "-i", "10000"] + self.args,
            stdout=subprocess.PIPE,
        )

        if self.pid < 0:
            austin_process = Process(self.austin.pid)
            while not austin_process.children():
                pass
            child_process = austin_process.children()[0]
            self.pid = child_process.pid
        else:
            child_process = Process(self.pid)

        self.cmd_line = child_process.cmdline()

        self.start_event.set()

        while True:
            line = self.austin.stdout.readline()
            if not line:
                break
            self.stats.add_thread_sample(bytes(line))

        self.quit_event.set()
