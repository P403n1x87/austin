import asyncio
import curses
import time

from austin import AsyncAustin
from austin.stats import Stats
from austin.widget import Label, Line, Pad, Window

# Widget positions
TITLE_LINE = 0
PROC_LINE = TITLE_LINE + 2
THREAD_LINE = PROC_LINE + 1
SAMPLES_LINE = THREAD_LINE + 1
TABHEAD_LINE = THREAD_LINE + 2
TAB_START = TABHEAD_LINE + 1


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

class AustinTUI(Window):

    def __init__(self, *args):
        super().__init__(*args)

        self.austin = AsyncAustin(self.on_sample_received)
        self.stats = Stats()

        self.current_threads = None
        self.current_thread = None
        self.current_thread_index = None
        self.duration = 0
        self.is_full_view = False

        # ---- Header ---------------------------------------------------------

        self.add_child("title_line", Line(
            TITLE_LINE, 0,
            "Austin -- Frame stack sampler for CPython."
        ))

        self.add_child("pid_label", Label(PROC_LINE, 0, "PID"))
        self.add_child("pid", Label(
            PROC_LINE, 5,
            lambda: "{:5}".format(self.austin.get_pid()),
            curses.A_BOLD
        ))

        self.add_child("cmd_line_label", Label(PROC_LINE, 12, "CMD"))
        self.add_child("cmd_line", Label(
            PROC_LINE, 17,
            lambda: self.austin.get_cmd_line(),
            curses.A_BOLD
        ))

        self.add_child("thread_line", Line(
            THREAD_LINE, 0,
            "Sampling ..."
        ))
        self.add_child("thread_num", Label(
            THREAD_LINE, 24,
            lambda: "{:^5}".format(self.current_thread_index + 1 if self.current_thread_index is not None else ""),
            attr=curses.A_REVERSE
        ))

        self.add_child("samples_line", Line(SAMPLES_LINE, 0))
        self.add_child("samples_count_label", Label(
            SAMPLES_LINE, 8,
            lambda: "{:8}".format(self.stats.samples),
            attr = curses.A_BOLD
        ))
        self.add_child("duration_label", Label(
            SAMPLES_LINE, 28,
            lambda: "{:>8}".format(fmt_time(int(self.duration))),
            attr = curses.A_BOLD
        ))

        # ---- Table ----------------------------------------------------------

        self.add_child("table_header", Line(
            TABHEAD_LINE, 0,
            " {:^6}  {:^6}  {:^6}  {:^6}  {}".format(
                "OWN",
                "TOTAL",
                "%OWN",
                "%TOTAL",
                "FUNCTION"
                ),
            curses.A_REVERSE | curses.A_BOLD
        ))
        self.add_child("table_pad", Pad(
            size_policy=lambda: [(h - TAB_START - 1, w) for h, w in [self.get_size()]][0],
            position_policy=lambda: (TAB_START, 0),
        ))

        # ---- Footer ---------------------------------------------------------
        #
        # TODO

        # Conect signal handlers
        self.connect("q", self.on_quit)
        self.connect("f", self.on_full_mode_toggled)
        self.connect("KEY_NPAGE", self.on_pgdown)
        self.connect("KEY_PPAGE", self.on_pgup)

    def __enter__(self):
        super().__enter__()

        curses.init_pair(1, 246, -1)

        return self

    def on_sample_received(self, line):
        self.stats.add_thread_sample(line.encode())

    def on_quit(self):
        raise KeyboardInterrupt("Quit signal")

    def on_pgdown(self):
        if self.current_threads:
            if self.current_thread_index < len(self.current_threads) - 1:
                self.current_thread_index += 1
                self.current_thread = \
                    self.current_threads[self.current_thread_index]
                self.thread_num.refresh()
                # self.table_pad.curr_y = 0
                self.update_thread_view()

    def on_pgup(self):
        if self.current_threads:
            if self.current_thread_index > 0:
                self.current_thread_index -= 1
                self.current_thread = \
                    self.current_threads[self.current_thread_index]
                self.thread_num.refresh()
                # self.table_pad.curr_y = 0
                self.update_thread_view()

    def on_full_mode_toggled(self):
        self.is_full_view = not self.is_full_view
        self.update_thread_view()

    def draw_ui(self, scr):
        h, w = scr.getmaxyx()


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
            self.table_pad.set_size(
                max(len(stack), h - TAB_START - 1),
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
            name_len = w - len(tail) - 34

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
        tab_h, tab_w = max(len(line_store), h - TAB_START - 1), w
        self.table_pad.set_size(
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

    def update_thread_view(self):
        scr = self.get_screen()

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

        self.table_pad.clear()

        if self.is_full_view:
            self.full_view(scr, thread)
        else:
            self.current_view(scr, thread)

            # scr.refresh()

        self.table_pad.refresh()


    def update_view(self, scr):
        self.update_thread_view()

        self.thread_line.set_text("{:29} of {:^5}".format(
            self.current_thread if self.current_thread else "",
            len(self.current_threads),
        ))

        self.duration = time.time() - self.start_time
        self.samples_line.set_text("Samples: {:8}  Duration: {:>8}".format(
            " ",
            " "
        ))
        self.samples_count_label.set_text("{:8}".format(self.stats.samples))
        self.duration_label.set_text(
            "{:>8}".format(fmt_time(int(self.duration)))
        )

    def run(self, scr):
        self.draw_ui(scr)

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
                except:
                    import traceback
                    self.last_error = repr(traceback.format_exc())

                await asyncio.sleep(.015)

        async def update_loop():
            while True:
                try:
                    # import pdb; pdb.set_trace()
                    scr.refresh()
                    self.update_view(scr)
                    self.refresh()
                except Exception as e:
                    import traceback
                    self.last_error = repr(traceback.format_exc())
                    raise e
                await asyncio.sleep(1)

        try:
            self.austin.get_event_loop().run_until_complete(
                asyncio.wait(
                    (input_loop(), update_loop()),
                    return_when=asyncio.FIRST_EXCEPTION
                )
            )
        except asyncio.CancelledError:
            pass

    def start(self, args):
        # Fork Austin
        self.austin.start(args)
        self.refresh()
        if self.austin.wait(1):
            self.run(self._scr)
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
