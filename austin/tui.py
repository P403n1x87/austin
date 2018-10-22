import curses
import time

import stats
from austin import Austin


TITLE_LINE = 0
PROC_LINE = TITLE_LINE + 2
THREAD_LINE = PROC_LINE + 1
SAMPLES_LINE = THREAD_LINE + 1
TABHEAD_LINE = THREAD_LINE + 2


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
        return f + " " + rest[:(length >> 1)-2] + "..." + rest[-(length >> 1)+1:]

    return f


class Label:
    def __init__(self, scr, y, x, text=None, attr=0):
        self.scr = scr
        self.x = x
        self.y = y
        self.attr = attr
        if text:
            self.set_text(text)

    def set_text(self, text, attr=None):
        if not attr:
            attr = self.attr

        self.scr.addstr(self.y, self.x, text, attr)

        return attr


class Line(Label):
    def set_text(self, text, attr=None):
        self.scr.chgat(super().set_text(text, attr))


def command_list(scr, y, cmd_list):
    x = 1

    for label, key in cmd_list.items():
        scr.addstr(y, x, key, curses.A_REVERSE)
        x += len(key) + 1
        scr.addstr(y, x, label)
        x += len(label) + 2
        if x > curses.COLS:
            scr.chgat(0)
            x = 1
            y += 1


class AustinTUI:

    def __init__(self, args):
        super().__init__()

        self.args = args
        self.current_threads = None
        self.current_thread = None
        self.current_thread_index = 0

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

        duration = time.time() - self.start_time
        self.samples_line.set_text("Samples: {:8}  Duration: {:8}s".format(
            self.stats.samples,
            int(duration)
        ))

        # Reverse the stack (top frames first)
        stack = stacks[thread][::-1]

        i = 1  # Keep track of the line number
        for frame in stack:
            writeln(
                scr,
                TABHEAD_LINE + i, 1,
                "{:5.2f}s  {:5.2f}s  {:5.2f}%  {:5.2f}%  {}".format(
                    frame["own_time"] / 1e6,
                    frame["tot_time"] / 1e6,
                    frame["own_time"] / 1e4 / duration,
                    frame["tot_time"] / 1e4 / duration,
                    ellipsis(frame["function"], curses.COLS - 34)
                )
            )

            # Don't overfill the screen
            # TODO: Consider using a widget for this (Pod?)
            if i > curses.LINES - TABHEAD_LINE - 2:
                break
            i += 1

        # Clean lines from previous sample
        if i < self.max_line:
            while i < self.max_line:
                writeln(scr, TABHEAD_LINE + i, 1, " ")
                i += 1

        self.max_line = i

    def handle_keypress(self, scr):
        try:
            key = scr.getkey()
            if key == "q":
                return -1

            if key == "KEY_NPAGE" and self.current_threads:
                if self.current_thread_index < len(self.current_threads) - 1:
                    self.current_thread_index += 1
                    self.current_thread = self.current_threads[self.current_thread_index]

            elif key == "KEY_PPAGE" and self.current_threads:
                if self.current_thread_index > 0:
                    self.current_thread_index -= 1
                    self.current_thread = self.current_threads[self.current_thread_index]

            return 0
        except curses.error:
            return 1

    def run(self, scr):
        curses.curs_set(0)
        scr.clear()
        scr.timeout(1000)

        # ---- Header ---------------------------------------------------------

        Line(scr, TITLE_LINE, 0, "Austin -- Frame stack sampler for CPython.")
        Line(scr, PROC_LINE, 0, "PID: {:5}  Cmd: {}".format(
            self.austin.get_pid(),
            self.austin.get_cmd_line()
        ))

        self.thread_line = Line(scr, THREAD_LINE, 0, "Sampling...")
        self.thread_num = Label(scr, THREAD_LINE, 24, attr=curses.A_REVERSE)

        self.samples_line = Line(scr, SAMPLES_LINE, 0)

        # ---- Table ----------------------------------------------------------

        Line(scr, TABHEAD_LINE, 0, "{:^6}  {:^6}  {:^6}  {:^6}  {}".format(
            "OWN",
            "TOTAL",
            "%OWN",
            "%TOTAL",
            "FUNCTION"
        ), curses.A_REVERSE | curses.A_BOLD)

        # ---- Footer ---------------------------------------------------------

        command_list(scr, curses.LINES - 1, {
            "Exit": " Q ",
            "PrevThread": "PgUp",
            "NextThread": "PgDn",
        })

        scr.refresh()

        # Prepare to refresh the screen
        self.max_line = 0  # Keep track of the number of rows written previously
        self.start_time = time.time()  # Keep track of the duration

        while self.austin.is_alive():
            # NOTE: This might not be required
            if self.austin.quit_event.wait(0):
                break

            self.update_view(scr)

            # Process user input
            if self.handle_keypress(scr) < 0:
                break

        # We are about to shut down
        if self.austin.is_alive():
            scr.addstr(curses.LINES - 1, 1, "Waiting for process to terminate...")
            scr.clrtoeol()
            scr.refresh()

            self.austin.join()
        else:
            scr.addstr(curses.LINES - 1, 1, "Process terminated")

    def start(self):
        self.stats = stats.Stats()

        self.austin = Austin(
            self.stats,
            self.args
        )
        self.austin.start()

        if (self.austin.start_event.wait(1)):
            try:
                curses.wrapper(self.run)
            except e:
                print(e)
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
