import curses

import stats
from austin import Austin


TITLE_LINE = 0
PROC_LINE = TITLE_LINE + 2
THREAD_LINE = PROC_LINE + 1
TABHEAD_LINE = THREAD_LINE + 2


def writeln(scr, *args, **kwargs):
    scr.addstr(*args, **kwargs)
    scr.clrtoeol()


class AustinTUI:

    def __init__(self, args):
        super().__init__()

        self.args = args

    def run(self, scr):
        curses.curs_set(0)
        scr.clear()
        scr.timeout(1000)

        scr.addstr(TITLE_LINE, 1, "Austin -- Frame stack sampler for CPython.")

        scr.addstr(PROC_LINE, 1, "PID: {:5}".format(self.austin.get_pid()))
        scr.addstr(PROC_LINE, 12, "CmdLine: " + self.austin.get_cmd_line())

        scr.addstr(THREAD_LINE, 1, "Sampling...")

        scr.addstr(TABHEAD_LINE, 0, " " * curses.COLS, curses.A_REVERSE | curses.A_BOLD)
        scr.addstr(TABHEAD_LINE, 1, "{:^6} {:^6}  {}".format(
            "OWN",
            "TOT",
            "FUNCTION"
        ), curses.A_REVERSE)

        scr.refresh()

        max_line = 0

        while self.austin.is_alive():
            # NOTE: This might not be required
            if self.austin.quit_event.wait(0):
                break

            stacks = self.stats.get_current_stacks()

            for thread in stacks:

                scr.addstr(THREAD_LINE, 1, thread)

                scr.addstr(THREAD_LINE, 24, "{:^5}".format(1), curses.A_REVERSE)
                scr.addstr(THREAD_LINE, 31, " of {:^5}".format(len(stacks)))

                stack = stacks[thread][::-1]

                i = 1
                for frame in stack:
                    writeln(scr, TABHEAD_LINE + i, 1, "{:6.2f} {:6.2f}  {}".format(
                        frame["own_time"] / 1e6,
                        frame["tot_time"] / 1e6,
                        frame["function"][:curses.COLS - 24]
                    ))

                    # Don't overfill the screen
                    # TODO: Consider using a widget for this (Pod?)
                    if i > curses.LINES - TABHEAD_LINE - 2:
                        break
                    i += 1

                # Clean lines from previous sample
                if i < max_line:
                    while i < max_line:
                        writeln(scr, TABHEAD_LINE + i, 1, " ")
                        i += 1

                max_line = i

                # TODO: This break is in place to only show one thread
                break

            # Process user input
            try:
                key = scr.getkey()
                if key == "q":
                    break
            except curses.error:
                pass

        # We are about to shut down
        if self.austin.is_alive():
            scr.addstr(curses.LINES - 1, 1, "Waiting for process to terminate...")
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
            curses.wrapper(self.run)
        else:
            print("Austin took too long to start. Terminating...")
            exit(1)
