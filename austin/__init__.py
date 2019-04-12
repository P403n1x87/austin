import asyncio
import sys

import psutil


# TODO: Create Base class
# TODO: Create Threaded class


class AsyncAustin:
    def __init__(self, args):
        self.args = args
        self.__loop = None
        self.start_event = asyncio.Event()
        self.__pid = int(args[1]) if args[0] == '-p' else -1
        self.__cmd_line = "<unknown>"

    def start(self, loop=None):
        async def _start():
            self.proc = await asyncio.create_subprocess_exec(
                "austin", *self.args, stdout=asyncio.subprocess.PIPE
            )

            if self.__pid < 0:  # Austin is forking
                austin_process = psutil.Process(self.austin.pid)
                while not austin_process.children():
                    pass
                child_process = austin_process.children()[0]
                self.__pid = child_process.pid
            else:  # Austin is attaching
                child_process = psutil.Process(self.__pid)

            self.__cmd_line = " ".join(child_process.cmdline())

            # Signal that we are good to go
            self.start_event.set()

            # Start readline loop
            while True:
                data = await self.proc.stdout.readline()
                if not data:
                    break
                self.on_sample_received(data.decode('ascii').rstrip())

            # Wait for the subprocess exit
            await self.proc.wait()

        if not loop:
            if sys.platform == "win32":
                self.__loop = asyncio.ProactorEventLoop()
                asyncio.set_event_loop(loop)
            else:
                self.__loop = asyncio.get_event_loop()
        else:
            self.__loop = loop

        self.__loop.create_task(_start())

    def get_event_loop(self):
        return self.__loop

    def get_pid(self):
        return self.__pid

    def get_cmd_line(self):
        return self.__cmd_line

    def wait(self, event, timeout=1):
        try:
            self.__loop.run_until_complete(
                asyncio.wait_for(event.wait(), timeout)
            )
        except asyncio.TimeoutError:
            return False

        return True


if __name__ == "__main__":
    class MyAsyncAustin(AsyncAustin):
        def on_sample_received(self, line):
            print(line)

    try:
        austin = MyAsyncAustin(sys.argv[1:])
        austin.start()
        austin.get_event_loop().run_forever()
    except KeyboardInterrupt:
        print("Bye!")
