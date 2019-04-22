import curses
import sys

from austin.tui import AustinTUI


def main(scr):
    args = sys.argv
    if len(args) == 1:
        print("Usage: austin-tui  command  [ARG...]")
        exit(1)

    with AustinTUI(scr) as austin_tui:
        austin_tui.start(args[1:])


if __name__ == "__main__":
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass
