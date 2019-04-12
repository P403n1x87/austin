import asyncio
import curses
import time

from austin import AsyncAustin
from austin.stats import Stats
from austin.widget import Label, Line, Pad

# Widget positions
TITLE_LINE = 0
PROC_LINE = TITLE_LINE + 2
THREAD_LINE = PROC_LINE + 1
SAMPLES_LINE = THREAD_LINE + 1
TABHEAD_LINE = THREAD_LINE + 2


# ---- Local Helpers ----------------------------------------------------------

def writeln(scr, *args, **kwargs):
    scr.addstr(*args, **kwargs)
    scr.clrtoeol()


def ellipsis(text, length):
    if len(text) <= length:
        return text

    try:
        f, rest = text.split()
    except ValueError:
        f, rest = text, ""

    if len(f) > length:
        return f[:length-3]+"..."

    if len(f) + 6 <= length:
        length -= len(f) + 1
        return f + " " + rest[:(length >> 1)-2] + "..." + \
            rest[-(length >> 1) + 1:]

    return f


def fmt_time(s):
    m = int(s // 60)
    ret = '{:02d}"'.format(round(s) % 60)
    if m:
        ret = str(m) + "'" + ret

    return ret


def command_list(scr, y, cmd_list):
    x = 1

    for label, key in cmd_list.items():
        scr.addstr(y, x, key, curses.A_REVERSE)
        x += len(key) + 1
        scr.addstr(y, x, label)
        x += len(label) + 2

        _, w = scr.getmaxyx()
        if x > w:
            scr.chgat(0)
            x = 1
            y += 1

    scr.chgat(0)


# ---- AustinTUI --------------------------------------------------------------

class AustinTUI(AsyncAustin):

    def __init__(self, args):
        super().__init__(args)

        self.args = args
        self.current_threads = None
        self.current_thread = None
        self.current_thread_index = 0
        self.is_full_view = False
        self.stats = None

    def on_sample_received(self, line):
        self.stats.add_thread_sample(line.encode())

    def draw_ui(self, scr):
        h, w = scr.getmaxyx()

        # ---- Header ---------------------------------------------------------

        Line(scr, TITLE_LINE, 0, "Austin -- Frame stack sampler for CPython.")

        Label(scr, PROC_LINE, 0, "PID:")
        self.pid_label = Label(scr, PROC_LINE, 5, "{:5}".format(
            self.get_pid()
        ), curses.A_BOLD)

        Label(scr, PROC_LINE, 12, "Cmd:")
        self.cmd_line = Label(
            scr, PROC_LINE, 17, self.get_cmd_line(), curses.A_BOLD
        )

        self.thread_line = Line(scr, THREAD_LINE, 0, "Sampling ...")
        self.thread_num = Label(scr, THREAD_LINE, 24, attr=curses.A_REVERSE)

        self.samples_line = Line(scr, SAMPLES_LINE, 0)
        self.samples_count_label = Label(
            scr, SAMPLES_LINE, 8, attr = curses.A_BOLD
        )
        self.duration_label = Label(
            scr, SAMPLES_LINE, 28, attr = curses.A_BOLD
        )

        # ---- Table ----------------------------------------------------------

        self.table_header = Line(
            scr,
            TABHEAD_LINE, 0,
            " {:^6}  {:^6}  {:^6}  {:^6}  {}".format(
                "OWN",
                "TOTAL",
                "%OWN",
                "%TOTAL",
                "FUNCTION"
                ),
            curses.A_REVERSE | curses.A_BOLD
        )

        self.table_pad = Pad(h - TABHEAD_LINE, w)

        # ---- Footer ---------------------------------------------------------

        command_list(scr, h - 1, {
            "Exit": " Q ",
            "PrevThread": "PgUp",
            "NextThread": "PgDn",
            "ToggleFullView": " F ",
        })

        scr.refresh()

    def current_view(self, scr, thread):
        """Display the last sample only.

        This representation gives a live snapshot of what is happening.
        """
        stacks = self.stats.get_current_stacks()
        if not stacks:
            return

        # Reverse the stack (top frames first)
        stack = stacks[thread][::-1]
        if not stack:
            writeln(self.table_pad, 0, 1, "< Empty >")

        else:
            h, w = scr.getmaxyx()
            i = 0  # Keep track of the line number
            self.table_pad.resize(
                max(len(stack) + 1, h - TABHEAD_LINE - 1),
                w
            )
            for frame in stack:
                writeln(
                    self.table_pad,
                    i, 0,
                    " {:^6}  {:^6}  {:5.2f}%  {:5.2f}%  {}".format(
                        fmt_time(frame["own_time"] / 1e6),
                        fmt_time(frame["tot_time"] / 1e6),
                        frame["own_time"] / 1e4 / self.duration,
                        frame["tot_time"] / 1e4 / self.duration,
                        ellipsis(frame["function"], curses.COLS - 34)
                    )
                )

                # Don't overfill the screen
                # TODO: Consider using a widget for this (Pad?)
                # if i > curses.LINES - TABHEAD_LINE - 2:
                #     break
                # i += 1

    def full_view(self, scr, thread):
        """Show all samples per thread.

        This gives a comprehensive frame stack view with all the has
        happened since the initial observation.
        """
        def print_child(node, char, prefix):
            if not node:
                return

            tail = ("└" if node.children else "") + char + prefix
            name_len = w - len(tail) - 33

            line = (
                " {:^6}  {:^6}  {:5.2f}%  {:5.2f}%  {:" +
                str(name_len) + "}").format(
                    fmt_time(node.own_time / 1e6),
                    fmt_time(node.total_time / 1e6),
                    node.own_time / 1e4 / self.duration,
                    node.total_time / 1e4 / self.duration,
                    ellipsis(node.function, name_len - 1),
                )
            line_store.append((line, tail, getattr(node, "is_active", False)))

        def print_children(nodes, prefix=""):
            if not nodes:
                return

            for n in nodes[:-1]:
                print_child(n, "┤", prefix)
                print_children(n.children, "│" + prefix)

            if nodes[-1]:
                print_child(nodes[-1], "┐", prefix)
                print_children(nodes[-1].children, " " + prefix)

        stack = self.stats.get_thread_stack(thread)
        if not stack:
            return

        h, w = scr.getmaxyx()
        line_store = []

        print_children(stack)

        i = 0
        tab_h, tab_w = max(len(line_store) + 1, h - TABHEAD_LINE - 1), w
        self.table_pad.resize(
            tab_h,
            tab_w
        )
        if not line_store:
            writeln(self.table_pad, tab_h >> 1, (tab_w >> 1) - 4, "< Empty >")
            return

        for l, t, a in line_store[::-1]:
            self.table_pad.addstr(i, 0, l, curses.color_pair(1) if not a else 0)
            self.table_pad.addstr(i, len(l), t, curses.color_pair(1))
            i += 1

    def update_view(self, scr):
        self.current_threads = self.stats.get_current_threads()

        if not self.current_threads:
            return

        if (
            not self.current_thread or
            self.current_thread not in self.current_threads
        ):
            self.current_thread = self.current_threads[0]
            self.current_thread_index = 0

        thread = self.current_thread
        self.current_thread_index = self.current_threads.index(thread)

        self.thread_line.set_text("{:29} of {:^5}".format(
            thread,
            len(self.current_threads),
        ))
        self.thread_num.set_text("{:^5}".format(self.current_thread_index + 1))

        self.duration = time.time() - self.start_time
        self.samples_line.set_text("Samples: {:8}  Duration: {:>8}".format(
            " ",
            " "
        ))
        self.samples_count_label.set_text("{:8}".format(self.stats.samples))
        self.duration_label.set_text(
            "{:>8}".format(fmt_time(int(self.duration)))
        )

        self.table_pad.clear()

        if self.is_full_view:
            self.full_view(scr, thread)
        else:
            self.current_view(scr, thread)

        scr.refresh()

        h, w = scr.getmaxyx()
        self.table_pad.refresh(TABHEAD_LINE + 1, 0, h - 2, w - 1)

    # async def handle_keypress(self, scr):
    def handle_keypress(self, scr):
        try:
            # key = await self.get_event_loop().run_in_executor(None, self.table_pad.getkey)
            key = self.table_pad.getkey()
            if self.table_pad.handle_input(key):
                pass

            elif key == "q":
                return -1

            elif key == "f":
                self.is_full_view = not self.is_full_view

            elif key == "KEY_NPAGE" and self.current_threads:
                if self.current_thread_index < len(self.current_threads) - 1:
                    self.current_thread_index += 1
                    self.current_thread = \
                        self.current_threads[self.current_thread_index]
                else:
                    return 0

            elif key == "KEY_PPAGE" and self.current_threads:
                if self.current_thread_index > 0:
                    self.current_thread_index -= 1
                    self.current_thread = \
                        self.current_threads[self.current_thread_index]
                else:
                    return 0

            elif key == "KEY_RESIZE":
                self.draw_ui(scr)

            else:
                # Unhandled input
                return 1

            self.update_view(scr)

            return 0
        except curses.error:
            return 1

    def run(self, scr):
        # TODO: Make it a context manager
        curses.start_color()
        curses.use_default_colors()
        curses.init_pair(1, 246, -1)
        curses.curs_set(False)
        scr.clear()
        scr.timeout(0)  # non-blocking for async I/O
        scr.nodelay(True)

        self.draw_ui(scr)

        self.start_time = time.time()  # Keep track of the duration

        async def input_loop():
            while True:
                # Process user input
                # if await self.handle_keypress(scr) < 0:
                if self.handle_keypress(scr) < 0:
                    for task in asyncio.Task.all_tasks():
                        task.cancel()
                    return

                await asyncio.sleep(.015)

        async def update_loop():
            while True:
                self.update_view(scr)
                await asyncio.sleep(1)

        try:
            self.get_event_loop().run_until_complete(
                asyncio.wait(
                    (input_loop(), update_loop()),
                    return_when=asyncio.FIRST_EXCEPTION
                )
            )
        except asyncio.CancelledError:
            pass

        scr.clrtoeol()
        scr.refresh()

    def start(self):
        super().start()

        self.stats = Stats()

        if self.wait(self.start_event, 1):
            try:
                curses.wrapper(self.run)
            except KeyboardInterrupt:
                pass
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
