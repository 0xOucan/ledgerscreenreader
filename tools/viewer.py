#!/usr/bin/env python3
"""PC viewer for ledgerscreenreader.

Shows the mirrored screen streamed by the ESP32-S3 over its native USB port
and prints the firmware log in this terminal.

    pip install pyserial
    python3 tools/viewer.py            # auto-detects the ESP32-S3
    python3 tools/viewer.py --port /dev/ttyACM0 --scale 8

Keys: o = orientation, z = 2x zoom, i = invert, d = bus dump to the log,
      s = save screenshot (PNG), q / Esc = quit
"""
import argparse
import os
import queue
import sys
import threading
import time
import tkinter as tk

import serial
from serial.tools import list_ports

MAGIC = bytes([0xA5, 0x5A, 0xC3, 0x3C])
W, H = 128, 64
PAYLOAD = W * H // 8
PACKET = len(MAGIC) + PAYLOAD + 1
ESPRESSIF_VID = 0x303A


def find_port():
    for p in list_ports.comports():
        if p.vid == ESPRESSIF_VID:
            return p.device
    return None


class Link(threading.Thread):
    """Reads the serial stream, splits it into log text and frame packets,
    and reconnects automatically when the board resets."""

    def __init__(self, port, frames):
        super().__init__(daemon=True)
        self.port = port
        self.frames = frames
        self.ser = None
        self.lock = threading.Lock()
        self.running = True

    def send(self, cmd):
        with self.lock:
            if self.ser:
                try:
                    self.ser.write(cmd.encode())
                except serial.SerialException:
                    pass

    def run(self):
        while self.running:
            port = self.port or find_port()
            if not port:
                time.sleep(0.5)
                continue
            try:
                # DTR/RTS drive the ESP32-S3 reset/boot pins over native USB;
                # keep them released so opening the port doesn't reset the chip.
                ser = serial.Serial()
                ser.port, ser.baudrate, ser.timeout = port, 115200, 0.05
                ser.dtr = ser.rts = False
                ser.open()
            except serial.SerialException:
                time.sleep(0.5)
                continue
            print(f"[viewer] connected to {port}", file=sys.stderr)
            with self.lock:
                self.ser = ser
            self.send("r")  # ask for a frame right away
            try:
                self._pump(ser)
            except (serial.SerialException, OSError):
                pass
            with self.lock:
                self.ser = None
            ser.close()
            print("[viewer] disconnected, waiting for the board...", file=sys.stderr)
            time.sleep(0.5)

    def _pump(self, ser):
        buf = bytearray()
        while self.running:
            buf += ser.read(4096)
            while True:
                i = buf.find(MAGIC)
                if i < 0:
                    # flush text, but keep a possible partial magic at the end
                    keep = next((k for k in (3, 2, 1) if buf.endswith(MAGIC[:k])), 0)
                    self._text(buf[: len(buf) - keep])
                    del buf[: len(buf) - keep]
                    break
                self._text(buf[:i])
                del buf[:i]
                if len(buf) < PACKET:
                    break
                payload = bytes(buf[len(MAGIC) : len(MAGIC) + PAYLOAD])
                chk = 0
                for b in payload:
                    chk ^= b
                if chk == buf[PACKET - 1]:
                    self.frames.put(payload)
                    del buf[:PACKET]
                else:  # false magic inside text
                    self._text(buf[:1])
                    del buf[:1]

    @staticmethod
    def _text(data):
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


class Viewer:
    def __init__(self, link, frames, scale, fg, bg):
        self.link, self.frames, self.scale = link, frames, scale
        self.fg, self.bg = fg, bg
        self.last = None
        self.count, self.fps_t, self.fps = 0, time.time(), 0.0

        self.root = tk.Tk()
        self.root.title("ledgerscreenreader")
        self.root.configure(bg="#111")
        self.img = tk.PhotoImage(width=W, height=H)
        self.big = self.img.zoom(scale)
        self.label = tk.Label(self.root, image=self.big, bd=0, bg="#111")
        self.label.pack(padx=12, pady=12)

        bar = tk.Frame(self.root, bg="#111")
        bar.pack(fill="x", padx=12, pady=(0, 10))
        for text, key in (("Orient (o)", "o"), ("Zoom (z)", "z"), ("Invert (i)", "i"),
                          ("Dump (d)", "d"), ("Screenshot (s)", "s")):
            tk.Button(bar, text=text, command=lambda k=key: self.key(k)).pack(side="left", padx=2)
        self.status = tk.Label(bar, text="waiting for frames...", fg="#aaa", bg="#111")
        self.status.pack(side="right")

        self.root.bind("<Key>", lambda e: self.key(e.char))
        self.root.bind("<Escape>", lambda e: self.root.destroy())
        self.draw(bytes(PAYLOAD))
        self.root.after(15, self.poll)

    def key(self, k):
        if k in ("o", "z", "i", "d"):
            self.link.send(k)
        elif k == "s":
            self.screenshot()
        elif k == "q":
            self.root.destroy()

    def draw(self, payload):
        rows = []
        for y in range(H):
            page, bit = (y >> 3) * W, 1 << (y & 7)
            rows.append("{" + " ".join(self.fg if payload[page + x] & bit else self.bg
                                       for x in range(W)) + "}")
        self.img.put(" ".join(rows))
        self.big = self.img.zoom(self.scale)
        self.label.configure(image=self.big)

    def poll(self):
        latest = None
        while True:
            try:
                latest = self.frames.get_nowait()
            except queue.Empty:
                break
        if latest is not None:
            self.count += 1
            if latest != self.last:
                self.last = latest
                self.draw(latest)
        now = time.time()
        if now - self.fps_t >= 1.0:
            self.fps = self.count / (now - self.fps_t)
            self.count, self.fps_t = 0, now
            state = "connected" if self.link.ser else "no board"
            self.status.configure(text=f"{state} | {self.fps:.0f} frames/s")
        self.root.after(15, self.poll)

    def screenshot(self):
        os.makedirs("captures", exist_ok=True)
        path = time.strftime("captures/screen_%Y%m%d_%H%M%S.png")
        self.big.write(path, format="png")
        print(f"[viewer] saved {path}", file=sys.stderr)

    def run(self):
        self.root.mainloop()
        self.link.running = False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first Espressif USB device)")
    ap.add_argument("--scale", type=int, default=6, help="pixel zoom (default 6)")
    ap.add_argument("--fg", default="#e8f4ff", help="lit pixel color")
    ap.add_argument("--bg", default="#000000", help="dark pixel color")
    args = ap.parse_args()

    frames = queue.Queue()
    link = Link(args.port, frames)
    link.start()
    Viewer(link, frames, args.scale, args.fg, args.bg).run()


if __name__ == "__main__":
    main()
