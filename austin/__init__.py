import asyncio
import sys
from abc import ABC, abstractmethod

import psutil
from argparse import ArgumentParser

from threading import Thread, Event

import subprocess


class AustinArgumentParser(ArgumentParser):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        self.add_argument(
            "-a", "--alt-format",
            help="alternative collapsed stack sample format.",
            action="store_true",
        )

        self.add_argument(
            "-e", "--exclude-empty",
            help="do not output samples of threads with no frame stacks.",
            action="store_true",
        )

        self.add_argument(
            "-i", "--interval",
            help="Sampling interval (default is 500us).",
            type=int,
        )

        self.add_argument(
            "-p", "--pid",
            help="The the ID of the process to which Austin should attach.",
            type=int,
        )

        self.add_argument(
            "-s", "--sleepless",
            help="suppress idle samples.",
            action="store_true",
        )

        self.add_argument(
            "-t", "--timeout",
            help="Approximate start up wait time. Increase on slow machines (default is 100ms).",
            type=int,
        )

        self.add_argument(
            "command",
            nargs="?",
            help="The command to execute if no PID is provided, followed by its arguments."
        )

        self.add_argument(
            "args",
            nargs="*",
            help="The arguments for the command."
        )

    def parse_args(self, args):
        parsed_args = super().parse_args(args)

        if not parsed_args.pid and not parsed_args.command:
            raise RuntimeError("No PID or command given.")

        return parsed_args


class BaseAustin(ABC):
    def __init__(self, sample_callback=None):
        self._loop = None
        self._pid = -1
        self._cmd_line = "<unknown>"
        self._running = False

        try:
            self._callback = (
                sample_callback if sample_callback else self.on_sample_received
            )
        except AttributeError as e:
            raise RuntimeError(
                "No sample callback given or implemented."
            ) from e

    def post_process_start(self):
        if not self._pid or self._pid < 0:  # Austin is forking
            austin_process = psutil.Process(self.proc.pid)
            while not austin_process.children():
                pass
            child_process = austin_process.children()[0]
            self._pid = child_process.pid
        else:  # Austin is attaching
            child_process = psutil.Process(self._pid)

        self._child = child_process
        self._cmd_line = " ".join(child_process.cmdline())

    @abstractmethod
    def start(self, args):
        ...

    def get_pid(self):
        return self._pid

    def get_cmd_line(self):
        return self._cmd_line

    def is_running(self):
        return self._running

    def get_child(self):
        return self._child

    @abstractmethod
    def wait(self, timeout=1):
        ...


class AsyncAustin(BaseAustin):
    def __init__(self, sample_callback=None):
        super().__init__(sample_callback)
        self.start_event = asyncio.Event()

    def start(self, args, loop=None):
        async def _start():
            self.proc = await asyncio.create_subprocess_exec(
                "austin", *args, stdout=asyncio.subprocess.PIPE
            )

            self.post_process_start()

            # Signal that we are good to go
            self.start_event.set()
            self._running = True

            # Start readline loop
            while True:
                data = await self.proc.stdout.readline()
                if not data:
                    break
                self._callback(data.decode('ascii').rstrip())

            # Wait for the subprocess exit
            await self.proc.wait()
            self._running = False

        parsed_args = AustinArgumentParser().parse_args(args)

        try:
            self._pid = parsed_args.pid
        except AttributeError:
            self._pid = -1

        if not loop:
            if sys.platform == "win32":
                self._loop = asyncio.ProactorEventLoop()
                asyncio.set_event_loop(loop)
            else:
                self._loop = asyncio.get_event_loop()
        else:
            self._loop = loop

        self._start_task = self._loop.create_task(_start())

    def get_event_loop(self):
        return self._loop

    def wait(self, timeout=1):
        try:
            self._loop.run_until_complete(
                asyncio.wait_for(self.start_event.wait(), timeout)
            )
        except asyncio.TimeoutError:
            return False

        return True

    def join(self):
        self._loop.run_until_complete(self._start_task)


class ThreadedAustin(BaseAustin, Thread):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        Thread.__init__(self)

        self.start_event = Event()

    def run(self):
        self.start_event.set()
        self._running = True

        while True:
            line = self.proc.stdout.readline()
            if not line:
                break
            self._callback(line.decode('ascii').rstrip())

        self.proc.wait()
        self._running = False

    def start(self, args):
        parsed_args = AustinArgumentParser().parse_args(args)

        try:
            self._pid = parsed_args.pid
        except AttributeError:
            self._pid = -1

        self.proc = subprocess.Popen(
            ["austin"] + args,
            stdout=subprocess.PIPE,
        )

        self.post_process_start()

        Thread.start(self)

    def wait(self, timeout=1):
        self.start_event.wait(timeout)

    def join(self):
        self.proc.wait()


# ---- TEST -------------------------------------------------------------------

if __name__ == "__main__":
    class MyAsyncAustin(AsyncAustin):
        def on_sample_received(self, line):
            print(line)

    try:
        austin = MyAsyncAustin()
        austin.start(["-i", "10000", "python3", "test/target34.py"])
        austin.join()
    except KeyboardInterrupt:
        print("Bye!")


    class MyThreadedAustin(ThreadedAustin):
        def on_sample_received(self, line):
            print(line)

    try:
        austin = MyThreadedAustin()
        austin.start(["-i", "10000", "python3", "test/target34.py"])
        austin.join()
    except KeyboardInterrupt:
        print("Bye!")
