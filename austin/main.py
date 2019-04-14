import curses
import sys

from austin.tui import AustinTUI

last_error = "bing"


def main(scr):
    global last_error

    args = sys.argv
    if len(args) == 1:
        print("Usage: austin-tui  command  [ARG...]")
        exit(1)

    with AustinTUI(scr) as austin_tui:
        try:
            austin_tui.start(args[1:])
        except:
            import traceback
            last_error = traceback.format_exc()
            pass
        finally:
            last_error = austin_tui.last_error


if __name__ == "__main__":
    try:
        curses.wrapper(main)
    except KeyboardInterrupt:
        pass
    finally:
        # print(last_error)
        for l in last_error.split("\\n"):
            print(l)
