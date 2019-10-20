import asyncio
import curses
import time

from austin import AsyncAustin
from austin.stats import Stats
from austin.widget import BarPlot, CommandBar, Label, Line, Table, TaggedLabel, Window


# ---- Widget Positions -------------------------------------------------------

TITLE_LINE = (0, 0)
LOGO = (1, 0)
LOGO_WIDTH = 13
INFO_AREA_X = LOGO_WIDTH + 1

PID = (TITLE_LINE[0] + 1, INFO_AREA_X)

CMD_LINE = (PID[0], INFO_AREA_X + 12)

THREAD = (PID[0] + 1, INFO_AREA_X)
THREAD_NUM = (PID[0] + 1, INFO_AREA_X + 24)
THREAD_TOTAL = (PID[0] + 1, INFO_AREA_X + 31)

SAMPLES = (THREAD[0] + 1, INFO_AREA_X)

DURATION = (SAMPLES[0], INFO_AREA_X + 18)

CPU = (THREAD[0], INFO_AREA_X + 40)
CPU_PLOT = (THREAD[0], INFO_AREA_X + 54)

MEM = (SAMPLES[0], INFO_AREA_X + 40)
MEM_PLOT = (SAMPLES[0], INFO_AREA_X + 54)

TABHEAD_LINE = (THREAD[0] + 2, 0)
TAB_START = (TABHEAD_LINE[0] + 1, 0)

TABHEAD_TEMPLATE = " {:^6}  {:^6}  {:^6}  {:^6}  {}"
TABHEAD_FUNCTION_PAD = len(TABHEAD_TEMPLATE.format("", "", "", "", ""))


# ---- Color Palette ----------------------------------------------------------


class Color:
    INACTIVE = 1
    HEAT_ACTIVE = 10
    HEAT_INACTIVE = 20
    RUNNING = 2
    STOPPED = 3
    CPU = 4
    MEMORY = 5
    THREAD = 6


PALETTE = {
    Color.INACTIVE: (246, -1),
    Color.RUNNING: (10, -1),
    Color.STOPPED: (1, -1),
    Color.CPU: (curses.COLOR_BLUE, -1),  # 17
    Color.MEMORY: (curses.COLOR_GREEN, -1),  # 22
    Color.THREAD: (11, -1),  # 22
}


# ---- Local Helpers ----------------------------------------------------------


def ell(text, length, sep=".."):
    if len(text) <= length:
        return text

    if length <= len(sep):
        return sep[:length]

    m = length >> 1
    n = length - m
    a = len(sep) >> 1
    b = len(sep) - a

    return text[: n - b - 1] + sep + text[-m + a - 1 :]


def ellipsis(text, length):
    if len(text) <= length:
        return text

    try:
        f, rest = text.split()
    except ValueError:
        f, rest = text, ""

    if len(f) > length:
        return f[: length - 3] + "..."

    if len(f) + 6 <= length:
        length -= len(f) + 1
        return f + " " + rest[: (length >> 1) - 2] + "..." + rest[-(length >> 1) + 1 :]

    return f


def fmt_time(s):
    m = int(s // 60e6)
    ret = '{:02d}"'.format(round(s / 1e6) % 60)
    if m:
        ret = str(m) + "'" + ret

    return ret


def fmt_mem(s):
    return f"{int(s)>>10: 5d}"


def color_level(p, a=True):
    d = 10 if a else 20
    return curses.color_pair(d + int(p / 20))


# ---- AustinTUI --------------------------------------------------------------


class AustinTUI(Window):
    def __init__(self, *args):
        super().__init__(*args)

        self.austin = AsyncAustin(self.on_sample_received)
        self.stats = Stats()

        self.current_threads = None
        self.current_thread = None
        self.current_thread_index = None
        self.duration = 0
        self.max_memory = 0
        self.current_cpu = 0
        self.current_memory = 0
        self.is_full_view = False

        # ---- Logo -----------------------------------------------------------

        self.add_child("title_line", Line(*TITLE_LINE, "  Austin TUI", curses.A_BOLD))

        self.add_child(
            "logo",
            Label(
                *LOGO,
                ["  _________  ", "  ⎝__⎠ ⎝__⎠  "],
                lambda: curses.color_pair(
                    (Color.RUNNING if self.austin.is_running() else Color.STOPPED)
                )
                | curses.A_BOLD,
            ),
        )

        # ---- Process Information --------------------------------------------

        self.add_child(
            "pid",
            TaggedLabel(
                *PID,
                tag=(lambda: "PPID" if self.args.children else "PID", 0),
                text=lambda: "{:5}".format(self.austin.get_pid()),
                attr=curses.A_BOLD,
            ),
        )

        # ---- Command Line ---------------------------------------------------

        self.add_child(
            "cmd_line",
            TaggedLabel(
                *CMD_LINE,
                tag=("CMD", 0),
                text=lambda: ell(self.austin.get_cmd_line(), self.get_size()[1] - 19),
                attr=curses.A_BOLD,
            ),
        )

        # ---- Threads --------------------------------------------------------

        self.add_child(
            "thread_name",
            TaggedLabel(
                *THREAD,
                tag=(
                    lambda: (
                        "{}TID".format("PID:" if self.args.children else "")
                        if self.current_thread
                        else "Sampling ..."
                    ),
                    0,
                ),
                text=lambda: "{:24}".format(self.current_thread or ""),
                attr=curses.A_BOLD,
            ),
        )

        self.add_child(
            "thread_total",
            Label(
                *THREAD_TOTAL,
                lambda: "of {:^5}".format(
                    len(self.current_threads) if self.current_threads else 0
                ),
            ),
        )

        self.add_child(
            "thread_num",
            Label(
                *THREAD_NUM,
                lambda: "{:5}".format(
                    self.current_thread_index + 1
                    if self.current_thread_index is not None
                    else ""
                ),
                attr=curses.color_pair(Color.THREAD) | curses.A_BOLD,
            ),
        )

        # ---- Samples --------------------------------------------------------

        self.add_child(
            "samples",
            TaggedLabel(
                *SAMPLES,
                lambda: "{:8}".format(self.stats.samples),
                tag=("Samples", 0),
                attr=curses.A_BOLD,
            ),
        )

        # ---- Duration -------------------------------------------------------

        self.add_child(
            "duration",
            TaggedLabel(
                *DURATION,
                tag=("Duration", 0),
                text=lambda: "{:>8}".format(fmt_time(int(self.duration * 1e6))),
                attr=curses.A_BOLD,
            ),
        )

        # ---- CPU ------------------------------------------------------------

        self.add_child(
            "cpu",
            TaggedLabel(
                *CPU,
                tag=("CPU", 0),
                text=lambda: "{: >5}%".format(
                    self.current_cpu if self.austin.is_running() else ""
                ),
                attr=curses.A_BOLD,
            ),
        )

        self.add_child(
            "cpu_plot",
            BarPlot(*CPU_PLOT, scale=100, init=0, attr=curses.color_pair(Color.CPU)),
        )

        # ---- Memory ---------------------------------------------------------

        self.add_child(
            "mem",
            TaggedLabel(
                *MEM,
                tag=("MEM", 0),
                text=lambda: "{: >5}M".format(
                    self.current_memory if self.austin.is_running() else ""
                ),
                attr=curses.A_BOLD,
            ),
        )

        self.add_child(
            "mem_plot", BarPlot(*MEM_PLOT, init=0, attr=curses.color_pair(Color.MEMORY))
        )

        # ---- Footer ---------------------------------------------------------

        self.add_child(
            "cmd_bar",
            CommandBar(
                {
                    "Exit": " Q ",
                    "PrevThread": "PgUp",
                    "NextThread": "PgDn",
                    "ToggleFullView": " F ",
                }
            ),
        )

        # Conect signal handlers
        self.connect("q", self.on_quit)
        self.connect("f", self.on_full_mode_toggled)
        self.connect("KEY_NPAGE", self.on_pgdown)
        self.connect("KEY_PPAGE", self.on_pgup)

        # ---- Table ----------------------------------------------------------

        self.add_child(
            "table_header",
            Line(
                *TABHEAD_LINE,
                TABHEAD_TEMPLATE.format("OWN", "TOTAL", "%OWN", "%TOTAL", "FUNCTION"),
                curses.A_REVERSE | curses.A_BOLD,
            ),
        )
        self.add_child(
            "table_pad",
            Table(
                size_policy=lambda: [
                    (h - TAB_START[0] - self.cmd_bar.get_height(), w)
                    for h, w in [self.get_size()]
                ][0],
                position_policy=lambda: (TAB_START[0], 0),
                columns=[" {:^6} ", " {:^6} ", " {:5.2f}% ", " {:5.2f}% ", " {}"],
                data_policy=self.generate_data,
                hook=self.draw_tree,
            ),
        )

        # ---- END OF UI DEFINITION -------------------------------------------

    def __enter__(self):
        super().__enter__()

        for color, values in PALETTE.items():
            curses.init_pair(color, *values)

        j = Color.HEAT_ACTIVE
        for i in [-1, 226, 208, 202, 196]:
            curses.init_pair(j, i, -1)
            j += 1
        j = Color.HEAT_INACTIVE
        for i in [246, 100, 130, 94, 88]:
            curses.init_pair(j, i, -1)
            j += 1

        return self

    # ---- EVENT HANDLERS -----------------------------------------------------

    def on_sample_received(self, line):
        self.stats.add_thread_sample(line.encode())

    def on_quit(self):
        raise KeyboardInterrupt("Quit signal")

    def on_pgdown(self):
        if self.current_threads:
            if self.current_thread_index < len(self.current_threads) - 1:
                self.current_thread_index += 1
                self.current_thread = self.current_threads[self.current_thread_index]

                self.refresh()

    def on_pgup(self):
        if self.current_threads:
            if self.current_thread_index > 0:
                self.current_thread_index -= 1
                self.current_thread = self.current_threads[self.current_thread_index]

                self.refresh()

    def on_full_mode_toggled(self):
        self.is_full_view = not self.is_full_view
        self.table_pad.refresh()

    # ---- METHODS ------------------------------------------------------------

    def scale_time(self, time, active=True):
        ratio = time / 1e4 / self.duration
        return ratio, color_level(ratio, active)

    def scale_memory(self, memory, active=True):
        ratio = (memory >> 10) / self.max_memory * 100
        return ratio, color_level(ratio, active)

    def get_current_cpu(self):
        if not self.austin.is_running():
            return 0

        value = int(self.austin.get_child().cpu_percent())
        self.current_cpu = value
        return value

    def get_current_memory(self):
        if not self.austin.is_running():
            return 0

        value = self.austin.get_child().memory_full_info()[0] >> 20
        self.current_memory = value
        if value > self.max_memory:
            self.max_memory = value
        return value

    def current_data(self):
        stacks = self.stats.get_current_stacks()
        if not stacks:
            return []

        # Reverse the stack (top frames first)
        stack = stacks[self.current_thread][::-1]
        if not stack:
            return []

        _, w = self.table_pad.get_inner_size()

        return [
            (
                [self.formatter(frame["own_time"]), 0],
                [self.formatter(frame["tot_time"]), 0],
                self.scaler(frame["own_time"]),
                self.scaler(frame["tot_time"]),
                [
                    ellipsis(
                        frame["function"][:-1] + ":" + frame["line_number"] + ")"
                        if self.args.linenos
                        else frame["function"],
                        w - TABHEAD_FUNCTION_PAD,
                    ),
                    0,
                ],
            )
            for frame in stack
        ]

    def full_data(self):
        def add_child(node, level):
            if not node:
                return

            name_len = w - level - TABHEAD_FUNCTION_PAD

            a = getattr(node, "is_active", False)
            attr = curses.color_pair(1) if not a else 0
            line_store.append(
                (
                    [self.formatter(node.own_time), attr],
                    [self.formatter(node.total_time), attr],
                    self.scaler(node.own_time, a),
                    self.scaler(node.total_time, a),
                    [
                        ellipsis(
                            node.function[:-1] + ":" + node.line_number + ")"
                            if self.args.linenos
                            else node.function,
                            name_len - 1,
                        ),
                        attr,
                    ],
                )
            )

        def add_children(nodes, level=2):
            if not nodes:
                return

            for n in nodes:
                if not n:
                    continue
                add_child(n, level)
                add_children(n.children, level + 1)

        _, w = self.table_pad.get_inner_size()
        stack = self.stats.get_thread_stack(self.current_thread)
        if not stack:
            return []

        line_store = []

        add_children(stack)

        return line_store[::-1]

    def draw_tree(self, pad):
        def print_child(node, char, prefix):
            if not node:
                return

            tail = ("└" if node.children else "") + char + prefix
            name_len = w - len(tail) - 34

            line_store.append(tail)

        def print_children(nodes, prefix=""):
            if not nodes:
                return

            for n in nodes[:-1]:
                print_child(n, "┤", prefix)
                print_children(n.children, "│" + prefix)

            if nodes[-1]:
                print_child(nodes[-1], "┐", prefix)
                print_children(nodes[-1].children, " " + prefix)

        if not self.is_full_view:
            return

        stack = self.stats.get_thread_stack(self.current_thread)
        if not stack:
            return

        h, w = self.get_size()
        line_store = []

        print_children(stack)

        i = 0
        for l in line_store[::-1]:
            pad.addstr(i, w - 1 - len(l), l, curses.color_pair(1))
            i += 1

    def generate_data(self):
        self.current_threads = self.stats.get_current_threads()

        if not self.current_threads:
            return []

        if not self.current_thread or self.current_thread not in self.current_threads:
            self.current_thread = self.current_threads[0]
            self.current_thread_index = 0

        self.current_thread_index = self.current_threads.index(self.current_thread)

        if self.is_full_view:
            return self.full_data()
        else:
            return self.current_data()

    def run(self, scr):
        self.start_time = time.time()  # Keep track of the duration

        async def input_loop():
            while True:
                try:
                    scr.refresh()
                    self.dispatch(self.table_pad.getkey())
                except KeyboardInterrupt:
                    for task in asyncio.Task.all_tasks():
                        task.cancel()
                    return

                except curses.error:
                    pass

                await asyncio.sleep(0.015)

        async def update_loop():
            while True:
                self.duration = time.time() - self.start_time
                self.get_child("cpu_plot").push(self.get_current_cpu())
                self.get_child("mem_plot").push(self.get_current_memory())
                self.refresh()
                scr.refresh()

                if not self.austin.is_running():
                    break

                await asyncio.sleep(1)

        try:
            done, pending = self.austin.get_event_loop().run_until_complete(
                asyncio.wait(
                    (input_loop(), update_loop()), return_when=asyncio.FIRST_EXCEPTION
                )
            )
            for task in done:
                task.result()
        except asyncio.CancelledError:
            pass

    def start(self, args):
        # Fork Austin
        self.austin.start(args)
        self.args = args

        self.get_child("title_line").set_text(
            " Austin  TUI  {} Profile".format("Memory" if args.memory else "Time")
        )

        # Set scaler and formatter
        self.formatter, self.scaler = (
            (fmt_mem, self.scale_memory) if args.memory else (fmt_time, self.scale_time)
        )

        self.refresh()
        try:
            if self.austin.wait(1):
                self.run(self._scr)
            else:
                raise AustinError("Took too long to start.")
        finally:
            self.austin.join()
