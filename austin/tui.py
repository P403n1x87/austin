import asyncio
import curses
import time

from austin import AsyncAustin
from austin.stats import Stats
from austin.widget import BarPlot, CommandBar, Label, Line, Table, Window

# Widget positions
TITLE_LINE = 0
PROC_LINE = TITLE_LINE + 2
THREAD_LINE = PROC_LINE + 1
SAMPLES_LINE = THREAD_LINE + 1
TABHEAD_LINE = THREAD_LINE + 2
TAB_START = TABHEAD_LINE + 1

TABHEAD_TEMPLATE = " {:^6}  {:^6}  {:^6}  {:^6}  {}"
TABHEAD_FUNCTION_PAD = len(TABHEAD_TEMPLATE.format("", "", "", "", ""))


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


class Color:
    INACTIVE = 1
    HEAT_ACTIVE = 10
    HEAT_INACTIVE = 20
    RUNNING = 2
    STOPPED = 3
    CPU = 4
    MEMORY = 5


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

        # ---- Header ---------------------------------------------------------

        self.add_child("title_line", Line(TITLE_LINE, 0, "Austin TUI"))

        self.add_child("pid_label", Label(PROC_LINE, 0, "PID"))
        self.add_child(
            "pid",
            Label(
                PROC_LINE,
                5,
                lambda: "{:5}".format(self.austin.get_pid()),
                curses.A_BOLD,
            ),
        )
        self.add_child(
            "pid_status",
            Label(
                PROC_LINE,
                11,
                "◉",
                lambda: curses.color_pair(
                    Color.RUNNING if self.austin.is_running() else Color.STOPPED
                ),
            ),
        )

        self.add_child("cmd_line_label", Label(PROC_LINE, 14, "CMD"))
        self.add_child(
            "cmd_line",
            Label(
                PROC_LINE,
                19,
                lambda: ell(self.austin.get_cmd_line(), self.get_size()[1] - 19),
                curses.A_BOLD,
            ),
        )

        self.add_child(
            "thread_name",
            Label(
                THREAD_LINE,
                0,
                lambda: "{:24}".format(
                    self.current_thread if self.current_thread else "Sampling ..."
                ),
            ),
        )
        self.add_child(
            "thread_total",
            Label(
                THREAD_LINE,
                31,
                lambda: "of {:^5}".format(
                    len(self.current_threads) if self.current_threads else 0
                ),
            ),
        )

        self.add_child(
            "thread_num",
            Label(
                THREAD_LINE,
                24,
                lambda: "{:^5}".format(
                    self.current_thread_index + 1
                    if self.current_thread_index is not None
                    else ""
                ),
                attr=curses.A_REVERSE,
            ),
        )

        self.add_child("samples_label", Label(SAMPLES_LINE, 0, "Samples"))
        self.add_child(
            "samples_count",
            Label(
                SAMPLES_LINE,
                7,
                lambda: "{:8}".format(self.stats.samples),
                attr=curses.A_BOLD,
            ),
        )

        self.add_child("duration_label", Label(SAMPLES_LINE, 18, "Duration"))
        self.add_child(
            "duration",
            Label(
                SAMPLES_LINE,
                26,
                lambda: "{:>8}".format(fmt_time(int(self.duration * 1e6))),
                attr=curses.A_BOLD,
            ),
        )

        self.add_child("cpu_label", Label(THREAD_LINE, 40, "CPU"))
        self.add_child(
            "cpu",
            Label(
                THREAD_LINE,
                44,
                lambda: "{: >5} %".format(
                    self.current_cpu if self.austin.is_running() else ""
                ),
                attr=curses.A_BOLD,
            ),
        )
        self.add_child(
            "cpu_plot",
            BarPlot(
                THREAD_LINE, 54, scale=100, init=0, attr=curses.color_pair(Color.CPU)
            ),
        )

        self.add_child("mem_label", Label(SAMPLES_LINE, 40, "MEM"))
        self.add_child(
            "mem",
            Label(
                SAMPLES_LINE,
                44,
                lambda: "{: >5} M".format(
                    self.current_memory if self.austin.is_running() else ""
                ),
                attr=curses.A_BOLD,
            ),
        )
        self.add_child(
            "mem_plot",
            BarPlot(SAMPLES_LINE, 54, init=0, attr=curses.color_pair(Color.MEMORY)),
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
                TABHEAD_LINE,
                0,
                TABHEAD_TEMPLATE.format("OWN", "TOTAL", "%OWN", "%TOTAL", "FUNCTION"),
                curses.A_REVERSE | curses.A_BOLD,
            ),
        )
        self.add_child(
            "table_pad",
            Table(
                size_policy=lambda: [
                    (h - TAB_START - self.cmd_bar.get_height(), w)
                    for h, w in [self.get_size()]
                ][0],
                position_policy=lambda: (TAB_START, 0),
                columns=[" {:^6} ", " {:^6} ", " {:5.2f}% ", " {:5.2f}% ", " {}"],
                data_policy=self.generate_data,
                hook=self.draw_tree,
            ),
        )

        # ---- END OF UI DEFINITION -------------------------------------------

    def __enter__(self):
        super().__enter__()

        curses.init_pair(Color.INACTIVE, 246, -1)
        curses.init_pair(Color.RUNNING, 10, -1)
        curses.init_pair(Color.STOPPED, 1, -1)
        curses.init_pair(Color.CPU, curses.COLOR_BLUE, curses.COLOR_BLACK)
        curses.init_pair(Color.MEMORY, curses.COLOR_GREEN, curses.COLOR_BLACK)
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
                        frame["function"][:-1] + ":" + frame["line_number"] + ")",
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
            line = (
                [self.formatter(node.own_time), attr],
                [self.formatter(node.total_time), attr],
                self.scaler(node.own_time, a),
                self.scaler(node.total_time, a),
                [
                    ellipsis(
                        node.function[:-1] + ":" + node.line_number + ")", name_len - 1
                    ),
                    attr,
                ],
            )
            line_store.append(line)

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
            self.austin.get_event_loop().run_until_complete(
                asyncio.wait(
                    (input_loop(), update_loop()), return_when=asyncio.FIRST_EXCEPTION
                )
            )
        except asyncio.CancelledError:
            pass

    def start(self, args):
        # Fork Austin
        self.austin.start(args)
        self.args = args

        self.get_child("title_line").set_text(
            "Austin TUI | {} Profile".format("Memory" if args.memory else "Time")
        )

        # Set scaler and formatter
        self.formatter, self.scaler = (
            (fmt_mem, self.scale_memory) if args.memory else (fmt_time, self.scale_time)
        )

        self.refresh()
        if self.austin.wait(1):
            self.run(self._scr)
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
