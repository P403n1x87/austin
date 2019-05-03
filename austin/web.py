import asyncio
import copy
import json
import sys
import weakref
from os import environ as env

from aiohttp import WSMsgType, web
from aiohttp.test_utils import unused_port
from austin import AsyncAustin
from austin.html import load_site
from austin.stats import parse_line
from pyfiglet import Figlet


class WebFrame:

    __slots__ = ["name", "value", "children", "index", "parent", "height"]

    def __init__(self, name, value):
        self.name = name
        self.value = value
        self.children = []
        self.index = {}
        self.parent = None
        self.height = 0

    def __add__(self, other):
        if self.name != other.name:
            self.parent.add_child(other)
            self.parent.height = max(self.height, other.height) + 1
        else:
            self.value += other.value
            if other.height > self.height:
                self.height = other.height
            for child in other.children:
                try:
                    self.index[child.name] += child
                except KeyError:
                    self.add_child(child)

        return self

    def add_child(self, frame):
        self.index[frame.name] = frame
        frame.parent = self
        self.children.append(frame)

    @staticmethod
    def from_line(text):
        def build_frame(frames):
            name, *tail = frames
            frame = WebFrame(name, value)
            if tail:
                frame.add_child(build_frame(tail))
            else:
                frame.value = value
            return frame

        thread, frames, value = parse_line(text.encode())
        frame = WebFrame(thread, value)
        frame.height = len(frames) + 1
        if frames:
            frame.add_child(build_frame(frames))

        root = WebFrame.new_root()
        root.add_child(frame)
        root.value = frame.value
        root.height = frame.height + 1

        return root

    @staticmethod
    def new_root():
        return WebFrame("root", 0)

    def to_dict(self):
        # ---------------------------------------
        # Validation check
        # ---------------------------------------
        # s = 0
        # for c in self.children:
        #     s += c.value
        #
        # if s > self.value:
        #     raise RuntimeError("Invalid Frame")
        # ---------------------------------------

        return {
            "name": self.name,
            "value": self.value,
            "children": [c.to_dict() for c in self.children]
        }


class DataPool:
    def __init__(self, austin):
        self._austin = austin
        self.max = 0
        self.data = WebFrame.new_root()
        self.samples = 0

    def add(self, frame):
        self.data += frame
        self.samples += 1

    async def send(self, ws):
        data = self.data.to_dict()

        if self.data.height > self.max:
            self.max = self.data.height

        payload = {
            "type": "sample",
            "data": data,
            "height": self.max,
            "samples": self.samples,
            "cpu": self._austin.get_child().cpu_percent(),
            "memory": self._austin.get_child().memory_full_info()[0] >> 20
        }

        await ws.send_str(json.dumps(payload))

        self.data = WebFrame.new_root()


class WebAustin(AsyncAustin):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        self.data_pools = weakref.WeakSet()

    def on_sample_received(self, text):
        for data_pool in self.data_pools:
            data_pool.add(WebFrame.from_line(text))

    def new_data_pool(self):
        data_pool = DataPool(self)
        self.data_pools.add(data_pool)
        return data_pool

    def discard_data_pool(self, data_pool):
        self.data_pools.discard(data_pool)

    async def handle_home(self, request):
        return web.Response(
            body=self.html,
            content_type='text/html'
        )

    async def handle_websocket(self, request):
        ws = web.WebSocketResponse()
        await ws.prepare(request)

        data_pool = self.new_data_pool()

        payload = {
            "type": "info",
            "pid": self.get_pid(),
            "command": self.get_cmd_line(),
        }

        await ws.send_str(json.dumps(payload))

        async for msg in ws:
            await data_pool.send(ws)

        self.discard_data_pool(data_pool)

        return ws

    def start_server(self):
        app = web.Application()
        app.add_routes(
            [
                web.get('/', self.handle_home),
                web.get('/ws', self.handle_websocket)
            ]
        )

        port = int(env.get("WEBAUSTIN_PORT", 0)) or unused_port()
        host = env.get("WEBAUSTIN_HOST") or "localhost"

        print(Figlet(font="speed", width=240).renderText("* Web Austin *"))
        print(
            f"* Sampling process with PID {self.get_pid()} "
            f"({self.get_cmd_line()})"
        )
        print(
            f"* Web Austin is running on http://{host}:{port}. "
            "Press Ctrl+C to stop."
        )

        self.html = load_site()
        web.run_app(app, host=host, port=port, print=None)

    def start(self):
        super().start(sys.argv[1:])

        if self.wait():
            self.start_server()
        else:
            print("Unable to start Austin")
            exit(1)


def main():
    austin = WebAustin()
    austin.start()

if __name__ == "__main__":
    main()
