import sys
import time
import zlib
import struct

from Xlib import X, XK, display
from Xlib.ext import xtest

SCALE = 2
MARGIN = 8
MENU_BAR = 24


def find_window(root, title):
    for child in root.query_tree().children:
        try:
            name = child.get_wm_name()
        except Exception:
            name = None
        if name and name.startswith(title):
            geometry = child.get_geometry()
            return geometry.x, geometry.y
        found = find_window(child, title)
        if found:
            return found
    return None


class Driver:
    def __init__(self):
        self.display = display.Display()
        self.root = self.display.screen().root
        for attempt in range(100):
            origin = find_window(self.root, "Odo SH3")
            if origin:
                break
            time.sleep(0.1)
        else:
            raise SystemExit("no Odo SH3 window")
        self.x = origin[0] + MARGIN
        self.y = origin[1] + MENU_BAR + MARGIN

    def tap(self, x, y, hold=0.6):
        xtest.fake_input(self.display, X.MotionNotify, x=self.x + SCALE * x, y=self.y + SCALE * y)
        self.display.sync()
        time.sleep(0.1)
        xtest.fake_input(self.display, X.ButtonPress, 1)
        self.display.sync()
        time.sleep(hold)
        xtest.fake_input(self.display, X.ButtonRelease, 1)
        self.display.sync()
        time.sleep(1.5)

    def key(self, name, down=None):
        code = self.display.keysym_to_keycode(XK.string_to_keysym(name))
        events = [X.KeyPress, X.KeyRelease] if down is None else [X.KeyPress if down else X.KeyRelease]
        for event in events:
            xtest.fake_input(self.display, event, code)
            self.display.sync()
            time.sleep(0.1)

    def chord(self, modifier, name):
        self.key(modifier, True)
        self.key(name)
        self.key(modifier, False)
        time.sleep(0.5)

    def text(self, value):
        for character in value:
            self.key({" ": "space"}.get(character, character))

    def screenshot(self, path):
        screen = self.display.screen()
        width, height = screen.width_in_pixels, screen.height_in_pixels
        data = self.root.get_image(0, 0, width, height, X.ZPixmap, 0xFFFFFFFF).data
        rows = []
        for y in range(height):
            line = data[y * width * 4:(y + 1) * width * 4]
            row = bytearray(b"\0")
            for x in range(width):
                row += bytes((line[x * 4 + 2], line[x * 4 + 1], line[x * 4]))
            rows.append(bytes(row))

        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

        with open(path, "wb") as handle:
            handle.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                         chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def main():
    driver = Driver()
    time.sleep(30)
    driver.chord("Control_L", "Escape")
    time.sleep(1)
    driver.text("r")
    time.sleep(1.5)
    driver.text("cmd")
    driver.key("Return")
    time.sleep(3)
    driver.text("dir")
    driver.key("Return")
    time.sleep(2)
    driver.screenshot(sys.argv[1])
    for command in sys.argv[2:]:
        driver.text(command)
        driver.key("Return")
        time.sleep(5)


main()
