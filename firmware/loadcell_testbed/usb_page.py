#!/usr/bin/env python3
"""The pad's setup page on this computer, through the USB cable.

    python usb_page.py            # finds the board, serves http://localhost:8712
    python usb_page.py COM7       # or name the port
    python usb_page.py COM7 9000  # and the http port

Why: the page normally lives on the pad's own wifi network, and a PC that joins
that network loses its internet. With the pad plugged in by USB this serves the
same page from the sketch next to this file and relays every request down the
serial line instead ($J and $SET in the firmware), so it is the REAL box you
are looking at - live reading, real settings, real saves.

The port is opened with DTR and RTS held low so the board is not reset.
Firmware upload is the one thing that does not go through here: flash over
USB with arduino-cli, or use the page on the wifi.

Needs pyserial:  pip install pyserial
"""
import os
import queue
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

import serial
from serial.tools import list_ports

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, "loadcell_testbed.ino")


def page():
    """The page exactly as the board serves it: the PAGE[] raw string."""
    with open(SKETCH, encoding="utf-8") as f:
        ino = f.read()
    a, b = ino.find('R"HTML('), ino.find(')HTML"')
    if a < 0 or b < 0:
        raise RuntimeError("PAGE[] literal not found in the sketch")
    return ino[a + len('R"HTML('):b]


def find_port():
    ports = list(list_ports.comports())
    # CH340 / CP210x / FTDI: the USB-serial chips ESP32 devkits carry.
    for p in ports:
        if p.vid in (0x1A86, 0x10C4, 0x0403):
            return p.device
    return ports[0].device if ports else None


class Board:
    def __init__(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = 115200
        s.timeout = 0.1
        s.dtr = False          # both low BEFORE opening, or the board resets
        s.rts = False
        s.open()
        self.ser = s
        self.lock = threading.Lock()
        self.replies = queue.Queue()
        self.alive = True
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        """Pick '$R,<what>,<n>' + n bytes out of everything else on the line."""
        buf = b""
        head = re.compile(rb"\$R,(\w+),(\d+)")
        try:
            while True:
                buf += self.ser.read(4096)
                while True:
                    nl = buf.find(b"\n")
                    if nl < 0:
                        break
                    m = head.fullmatch(buf[:nl].rstrip(b"\r"))
                    if not m:
                        buf = buf[nl + 1:]          # console text, not for us
                        continue
                    need = nl + 1 + int(m.group(2))
                    if len(buf) < need:
                        break                        # body still arriving
                    self.replies.put((m.group(1).decode(), buf[nl + 1:need]))
                    buf = buf[need:]
        except (serial.SerialException, OSError):
            self.alive = False

    def send(self, line):
        with self.lock:
            self.ser.write((line + "\n").encode("utf-8"))

    def ask(self, line, tag, timeout=1.5):
        """Send a line, return the body of the matching reply, or None."""
        with self.lock:
            while not self.replies.empty():          # answers nobody waited for
                self.replies.get_nowait()
            self.ser.write((line + "\n").encode("utf-8"))
            end = time.time() + timeout
            while time.time() < end:
                try:
                    what, body = self.replies.get(timeout=max(0.01, end - time.time()))
                except queue.Empty:
                    return None
                if what == tag:
                    return body
            return None


def make_handler(board):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):                   # four requests a second
            pass

        def out(self, code, body, ctype="text/plain; charset=utf-8"):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def relay(self, line, tag, ctype):
            body = board.ask(line, tag)
            if body is None:
                # Busy taking a zero or a calibration, or not answering at all.
                return self.out(504, "no answer from the board")
            self.out(200, body, ctype)

        def do_GET(self):
            u = urlsplit(self.path)
            p, q = u.path, u.query
            js = "application/json"
            if p in ("/", "/index.html"):
                return self.out(200, page(), "text/html; charset=utf-8")
            if p == "/data":
                return self.relay("$J,data", "data", js)
            if p == "/info":
                return self.relay("$J,info", "info", js)
            if p == "/hits":
                if "clear" in q:
                    board.send("$HITLOG,clear")
                return self.relay("$J,hits", "hits", js)
            if p == "/doc":
                return self.relay("$J,doc", "doc", "text/plain; charset=utf-8")
            if p == "/log":
                m = re.search(r"since=(\d+)", q)
                return self.relay("$J,log,%s" % (m.group(1) if m else "0"), "log",
                                  "text/plain; charset=utf-8")
            if p == "/cmd":
                m = re.search(r"(?:^|&)c=([^&]*)", q)
                board.send(unquote(m.group(1).replace("+", " "))[:94] if m else "")
                return self.out(200, "ok")
            if p == "/set":
                return self.relay("$SET," + unquote(q)[:88], "set", "text/plain")
            self.out(404, "no")

        def do_POST(self):
            self.out(501, "Firmware upload does not go over the USB link. "
                          "Flash with arduino-cli, or use the page on the wifi.")
    return H


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    http_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8712
    if not port:
        sys.exit("No serial port found. Is the pad plugged in?")
    board = Board(port)
    time.sleep(0.3)
    if board.ask("$J,data", "data", 2.5) is None:
        print("The board on %s did not answer $J,data." % port)
        print("Either it is not the pad, or its firmware predates the USB link")
        print("(flash the current sketch first). Serving anyway.")
    else:
        print("pad found on %s" % port)
    srv = ThreadingHTTPServer(("127.0.0.1", http_port), make_handler(board))
    print("open  http://localhost:%d   (Ctrl+C to stop)" % http_port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
