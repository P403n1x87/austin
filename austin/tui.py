import curses
import time

import stats
from austin import Austin
from widget import Label, Line, Pad

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

    f, rest = text.split()

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

class AustinTUI:

    def __init__(self, args):
        super().__init__()

        self.args = args
        self.current_threads = None
        self.current_thread = None
        self.current_thread_index = 0
        self.is_full_view = False

    def draw_ui(self, scr):
        h, w = scr.getmaxyx()

        # ---- Header ---------------------------------------------------------

        Line(scr, TITLE_LINE, 0, "Austin -- Frame stack sampler for CPython.")

        Label(scr, PROC_LINE, 0, "PID:")
        self.pid_label = Label(scr, PROC_LINE, 5, "{:5}".format(
            self.austin.get_pid()
        ), curses.A_BOLD)

        Label(scr, PROC_LINE, 12, "Cmd:")
        self.pid_label = Label(
            scr, PROC_LINE, 17, self.austin.get_cmd_line(), curses.A_BOLD
        )

        self.thread_line = Line(scr, THREAD_LINE, 0, "")
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

    def current_view(self, scr, stacks, thread):
        """Display the last sample only.

        This representation gives a live snapshot of what is happening.
        """
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
                if i > curses.LINES - TABHEAD_LINE - 2:
                    break
                i += 1

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
                ) + tail
            line_store.append(line)

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
        self.table_pad.resize(
            max(len(line_store) + 1, h - TABHEAD_LINE - 1),
            w
        )
        if not line_store:
            writeln(self.table_pad, 0, 1, "< Empty >")
            return

        for l in line_store[::-1]:
            writeln(self.table_pad, i, 0, l)
            i += 1

    def update_view(self, scr):
        stacks = self.stats.get_current_stacks()
        if not stacks:
            return

        self.current_threads = sorted(stacks.keys())
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
            self.current_view(scr, stacks, thread)

        scr.refresh()

    def handle_keypress(self, scr):
        h, w = scr.getmaxyx()
        self.table_pad.refresh(TABHEAD_LINE + 1, 0, h - 2, w - 1)

        try:
            key = self.table_pad.getkey()

            if key == "q":
                return -1

            if key == "f":
                self.is_full_view = not self.is_full_view

            if self.table_pad.handle_input(key):
                return 0

            if key == "KEY_NPAGE" and self.current_threads:
                if self.current_thread_index < len(self.current_threads) - 1:
                    self.current_thread_index += 1
                    self.current_thread = \
                        self.current_threads[self.current_thread_index]

            elif key == "KEY_PPAGE" and self.current_threads:
                if self.current_thread_index > 0:
                    self.current_thread_index -= 1
                    self.current_thread = \
                        self.current_threads[self.current_thread_index]

            elif key == "KEY_RESIZE":
                self.draw_ui(scr)

            return 0
        except curses.error:
            return 1

    def run(self, scr):
        curses.use_default_colors()
        curses.curs_set(False)
        scr.clear()
        scr.timeout(1000)

        self.draw_ui(scr)

        # Prepare to refresh the screen
        self.thread_line.set_text("Sampling...")
        self.start_time = time.time()  # Keep track of the duration

        # ---- Main Loop ------------------------------------------------------

        while self.austin.is_alive():
            h, w = scr.getmaxyx()

            # NOTE: This might not be required
            if self.austin.quit_event.wait(0):
                break

            self.update_view(scr)

            # Process user input
            if self.handle_keypress(scr) < 0:
                break

        # We are about to shut down
        if self.austin.is_alive():
            scr.addstr(h - 1, 1, "Waiting for process to terminate...")
            scr.clrtoeol()
            scr.refresh()

            self.austin.join()
        else:
            scr.addstr(h - 1, 1, "Process terminated")

    def start(self):
        self.stats = stats.Stats()

        self.austin = Austin(
            self.stats,
            self.args
        )
        self.austin.start()

        if (self.austin.start_event.wait(1)):
            curses.wrapper(self.run)
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
