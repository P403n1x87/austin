import asyncio
import sys
from abc import ABC, abstractmethod

import psutil
from argparse import ArgumentParser

from threading import Thread, Event

import subprocess


class AustinError(Exception):
    pass


class AustinArgumentParser(ArgumentParser):
    def __init__(
        self,
        name="austin",
        alt_format=True,
        children=True,
        exclude_empty=True,
        full=True,
        interval=True,
        memory=True,
        pid=True,
        sleepless=True,
        timeout=True,
        command=True,
        **kwargs,
    ):
        super().__init__(prog=name, **kwargs)

        if bool(pid) != bool(command):
            raise RuntimeError(
                "Austin command line parser must have either pid or command."
            )

        if alt_format:
            self.add_argument(
                "-a",
                "--alt-format",
                help="Alternative collapsed stack sample format.",
                action="store_true",
            )

        if children:
            self.add_argument(
                "-C",
                "--children",
                help="Attach to child processes.",
                action="store_true",
            )

        if exclude_empty:
            self.add_argument(
                "-e",
                "--exclude-empty",
                help="Do not output samples of threads with no frame stacks.",
                action="store_true",
            )

        if full:
            self.add_argument(
                "-f",
                "--full",
                help="Produce the full set of metrics (time +mem -mem).",
                action="store_true",
            )

        if interval:
            self.add_argument(
                "-i",
                "--interval",
                help="Sampling interval (default is 100us).",
                type=int,
            )

        if memory:
            self.add_argument(
                "-m", "--memory", help="Profile memory usage.", action="store_true"
            )

        if pid:
            self.add_argument(
                "-p",
                "--pid",
                help="The the ID of the process to which Austin should attach.",
                type=int,
            )

        if sleepless:
            self.add_argument(
                "-s", "--sleepless", help="Suppress idle samples.", action="store_true"
            )

        if timeout:
            self.add_argument(
                "-t",
                "--timeout",
                help="Approximate start up wait time. Increase on slow machines (default is 100ms).",
                type=int,
            )

        if command:
            self.add_argument(
                "command",
                nargs="?",
                help="The command to execute if no PID is provided, followed by its arguments.",
            )

            self.add_argument(
                "args", nargs="*", help="Arguments to pass to the command to run."
            )

    def parse_args(self, args):
        parsed_args = super().parse_args(args)

        if not parsed_args.pid and not parsed_args.command:
            raise RuntimeError("No PID or command given.")

        return parsed_args

    @staticmethod
    def to_list(args):
        arg_list = []
        if getattr(args, "alt_format", None):
            arg_list.append("-a")
        if getattr(args, "children", None):
            arg_list.append("-C")
        if getattr(args, "exclude_empty", None):
            arg_list.append("-e")
        if getattr(args, "full", None):
            arg_list.append("-f")
        if getattr(args, "interval", None):
            arg_list += ["-i", str(args.interval)]
        if getattr(args, "memory", None):
            arg_list.append("-m")
        if getattr(args, "pid", None):
            arg_list += ["-p", str(args.pid)]
        if getattr(args, "sleepless", None):
            arg_list.append("-s")
        if getattr(args, "timeout", None):
            arg_list += ["-t", str(args.timeout)]
        if getattr(args, "command", None):
            arg_list.append(args.command)
        if getattr(args, "args", None):
            arg_list += args.args

        return arg_list


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
            raise RuntimeError("No sample callback given or implemented.") from e

    def post_process_start(self):
        if not self._pid or self._pid < 0:  # Austin is forking
            austin_process = psutil.Process(self.proc.pid)
            while not austin_process.children():
                pass
            child_process = austin_process.children()[0]
            if child_process.pid is not None:
                self._pid = child_process.pid
        else:  # Austin is attaching
            try:
                child_process = psutil.Process(self._pid)
            except psutil.NoSuchProcess:
                raise AustinError(
                    f"Cannot attach to process with PID {self._pid} because it does not seem to exist."
                )

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
            try:
                self.proc = await asyncio.create_subprocess_exec(
                    "austin",
                    *AustinArgumentParser.to_list(args),
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.DEVNULL,
                )
            except FileNotFoundError:
                raise AustinError("Executable not found.")

            self.post_process_start()

            # Signal that we are good to go
            self.start_event.set()
            self._running = True

            # Start readline loop
            while True:
                data = await self.proc.stdout.readline()
                if not data:
                    break
                self._callback(data.decode("ascii").rstrip())

            # Wait for the subprocess exit
            await self.proc.wait()
            self._running = False

        try:
            if args.pid is not None:
                self._pid = args.pid
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
        try:
            return self._loop.run_until_complete(self._start_task)
        except asyncio.CancelledError:
            pass


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
            self._callback(line.decode("ascii").rstrip())

        self.proc.wait()
        self._running = False

    def start(self, args):
        try:
            self._pid = args.pid
        except AttributeError:
            self._pid = -1

        self.proc = subprocess.Popen(["austin"] + args, stdout=subprocess.PIPE)

        try:
            self.post_process_start()
        except psutil.NoSuchProcess as e:
            raise AustinError("Unable to start Austin.") from e

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
