from collections import deque
import curses


class Widget:
    def __init__(self):
        self.parent = None
        self._children = {}
        self._event_handlers = {"KEY_RESIZE": self.on_resize}

    def __getattr__(self, name):
        try:
            return self._children[name]
        except KeyError:
            raise AttributeError(self, self.parent, name)

    def add_child(self, name, child):
        # if name in self._children:
        #     raise RuntimeError(f"Child {name} already exists.")

        child.parent = self
        self._children[name] = child

    def get_child(self, name):
        return self._children.get(name, None)

    def connect(self, event, handler):
        self._event_handlers[event] = handler

    def dispatch(self, event, *args, **kwargs):
        stop = False
        try:
            for _, child in self._children.items():
                try:
                    stop = child.dispatch(event, *args, **kwargs)
                    if stop:
                        break
                except AttributeError:
                    pass
            if not stop:
                self._event_handlers[event](*args, **kwargs)
        except KeyError as e:
            pass

    def on_resize(self):
        self.refresh()
        return True

    def get_toplevel(self):
        toplevel = self
        while toplevel.parent:
            toplevel = toplevel.parent

        return toplevel

    def refresh(self):
        for _, child in self._children.items():
            try:
                child.refresh()
            except AttributeError:
                pass


class Window(Widget):
    def __init__(self, screen):
        super().__init__()
        self._scr = screen

    def __enter__(self):
        curses.start_color()
        curses.use_default_colors()
        curses.curs_set(False)

        self._scr.clear()
        self._scr.timeout(0)  # non-blocking for async I/O
        self._scr.nodelay(True)

        return self

    def __exit__(self, *args):
        self._scr.clrtoeol()
        self._scr.refresh()

    def get_size(self):
        return self._scr.getmaxyx()

    def get_screen(self):
        return self._scr


class Label(Widget):
    def __init__(self, y, x, text=None, attr=0):
        super().__init__()

        self.x = x
        self.y = y
        self.attr = attr
        self.text = text

        self.scr = None

    def set_text(self, text, attr=None):
        self.text = text
        if attr:
            self.attr = attr

        self.refresh()

    def get_text(self):
        return self.current_text

    def refresh(self):
        if not self.scr:
            self.scr = self.get_toplevel().get_screen()
        self.current_text = self.text() if callable(self.text) else self.text
        self.current_attr = self.attr() if callable(self.attr) else self.attr
        if isinstance(self.current_text, list):
            for i, line in enumerate(self.current_text):
                self.scr.addstr(self.y + i, self.x, line or "", self.current_attr)
        else:
            self.scr.addstr(self.y, self.x, self.current_text or "", self.current_attr)


class TaggedLabel(Label):
    def __init__(self, y, x, text=None, tag=None, attr=0):
        super().__init__(y, x, text, attr)
        self.orig_x, self.orig_y = x, y

        self.add_child("tag", Label(y, x, *tag))

    def refresh(self):
        tag = self.get_child("tag")
        tag.refresh()
        self.x = self.orig_x + len(tag.get_text()) + 1
        super().refresh()
        self.scr.addstr(self.y, self.x - 1, " ")


class Line(Label):
    def refresh(self):
        super().refresh()

        self.scr.chgat(self.attr)


class Pad(Widget):
    def __init__(self, position_policy, size_policy):
        super().__init__()

        self.h, self.w = size_policy()

        self._sizep = size_policy
        self._posp = position_policy

        self.pad = curses.newpad(*size_policy())
        self.pad.scrollok(True)
        self.pad.keypad(True)
        self.pad.timeout(0)
        self.pad.nodelay(True)

        self.curr_y = 0
        self.curr_x = 0

        self.connect("KEY_UP", self.on_up)
        self.connect("KEY_DOWN", self.on_down)

    def __getattr__(self, name):
        return getattr(self.pad, name)

    def get_inner_size(self):
        h, w = self._sizep()

        return h, w - 1

    def on_down(self):
        h, _ = self._sizep()
        if self.curr_y + h < self.h:
            self.curr_y += 1
            self.refresh()

    def on_up(self):
        if self.curr_y > 0:
            self.curr_y -= 1
            self.refresh()

    def set_size(self, h, w):
        self.h, self.w = h, w
        self.pad.resize(h, w)  # Scroll bar

    def draw_scroll_bar(self):
        y0, x0 = self._posp()
        h, w = self._sizep()

        x = x0 + w - 1

        scr = self.get_toplevel().get_screen()
        for i in range(h):
            scr.addstr(y0 + i, x, "░")

        bar_h = min(int(h * h / self.h) + 1, h)
        if bar_h != h:
            bar_y = int(self.curr_y / self.h * h)
            for i in range(bar_h):
                scr.addstr(y0 + bar_y + i, x, "▓")

        scr.refresh()

    def refresh(self):
        super().refresh()

        h, w = self._sizep()

        if self.curr_y + h > self.h:
            self.curr_y = 0

        y1, x1 = self._posp()

        y2, x2 = y1 + h - 1, x1 + w - 1

        self.pad.refresh(self.curr_y, self.curr_x, y1, x1, y2, x2)
        self.draw_scroll_bar()


class Table(Pad):
    def __init__(self, position_policy, size_policy, columns, data_policy, hook=None):
        super().__init__(position_policy, size_policy)

        self._datap = data_policy
        self._cols = columns
        self._hook = hook

    def show_empty(self):
        h, w = self._sizep()
        self.pad.addstr(h >> 1, (w >> 1) - 4, "< Empty >")

    def set_row(self, i, row):
        x = 0
        for j in range(len(self._cols)):
            text, attr = row[j]
            text = self._cols[j].format(text)
            self.pad.addstr(i, x, text, attr)
            x += len(text)

    def refresh(self):
        data = self._datap()
        h, w = self._sizep()
        self.set_size(max(len(data), h), w)  # ???

        self.pad.clear()
        if not data:
            self.show_empty()
        else:
            i = 0
            for e in data:
                self.set_row(i, e)
                i += 1

            try:
                self._hook(self.pad)
            except AttributeError:
                pass

        super().refresh()


class CommandBar(Widget):
    def __init__(self, commands):
        super().__init__()

        self._cmds = commands
        self.scr = None
        self.h = 0

    def refresh(self):
        if not self.scr:
            self.scr = self.get_toplevel().get_screen()

        h, w = self.scr.getmaxyx()
        x, y = 1, h - 1

        for label, key in self._cmds.items():
            if x + len(key) + len(label) + 3 > w:
                self.scr.clrtoeol()
                self.scr.chgat(0)
                x = 1
                y -= 1

            try:
                self.scr.addstr(y, x, key, curses.A_REVERSE)
                x += len(key)
                self.scr.addstr(y, x, " " + label + "  ")
                x += len(label) + 2
            except curses.error:
                pass

        self.h = h - y

        self.scr.chgat(0)
        self.scr.clrtoeol()

    def get_height(self):
        return self.h


class BarPlot(Label):

    STEPS = [" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"]

    @staticmethod
    def bar_icon(i):
        i = max(0, min(i, 1))
        return BarPlot.STEPS[int(i * (len(BarPlot.STEPS) - 1))]

    def __init__(self, y, x, width=8, scale=None, init=None, attr=0):
        super().__init__(y, x, attr=attr)

        self._values = deque([init] * width if init is not None else [], maxlen=width)
        self.scale = scale or 0
        self.auto = not scale

    def push(self, value):
        self._values.append(value)
        if self.auto:
            self.scale = max(self._values)

        self.plot()

        return value

    def plot(self):
        self.set_text(
            "".join(
                BarPlot.bar_icon(v / self.scale if self.scale else v)
                for v in self._values
            )
        )
