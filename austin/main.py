import curses
import sys

from austin.tui import AustinTUI


def curses_app(scr):
    args = sys.argv
    if len(args) == 1:
        print("Usage: austin-tui  command  [ARG...]")
        exit(1)

    with AustinTUI(scr) as austin_tui:
        austin_tui.start(args[1:])


def main():
    try:
        curses.wrapper(curses_app)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
