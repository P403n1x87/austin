import curses
import sys

from austin import AustinArgumentParser, AustinError
from austin.tui import AustinTUI


def curses_app(scr, args):
    with AustinTUI(scr) as austin_tui:
        austin_tui.start(args)


def main():
    arg_parser = AustinArgumentParser(name="austin-tui", full=False, alt_format=False)

    arg_parser.add_argument(
        "-l", "--linenos", action="store_true", help="Show line numbers"
    )

    parsed_args = arg_parser.parse_args(sys.argv[1:])

    try:
        curses.wrapper(lambda scr: curses_app(scr, parsed_args))
    except KeyboardInterrupt:
        pass
    except AustinError as e:
        print(f"Cannot start Austin: {e}")
        exit(1)


if __name__ == "__main__":
    main()
