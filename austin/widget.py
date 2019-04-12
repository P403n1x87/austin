import curses


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


class Pad:
    def __init__(self, h, w):
        self.h, self.w = h, w
        self.pad = curses.newpad(h, w)
        self.pad.scrollok(True)
        self.pad.keypad(True)
        self.pad.timeout(0)
        self.pad.nodelay(True)

        self.curr_y = 0
        self.curr_x = 0

    def __getattr__(self, name):
        return getattr(self.pad, name)

    def handle_input(self, k):
        if k == "KEY_DOWN":
            self.curr_y += 1

        elif k == "KEY_UP" and self.curr_y > 0:
            self.curr_y -= 1

        else:
            return False

        return True

    def resize(self, h, w):
        self.h, self.w = h, w
        self.pad.resize(h, w)

    def refresh(self, *args):
        y1, x1, y2, x2 = args

        w, h = x2 - x1, y2 - y1

        if self.curr_y + h > self.h - 2:
            self.curr_y = self.h - h - 2
        else:
            self.pad.refresh(self.curr_y, self.curr_x, *args)
