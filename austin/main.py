import sys

from austin.tui import AustinTUI


def main():
    args = sys.argv
    if len(args) == 1:
        print("Usage: austin-tui  command  [ARG...]")
        exit(1)

    austin_tui = AustinTUI(args[1:])
    austin_tui.start()


if __name__ == "__main__":
    main()
