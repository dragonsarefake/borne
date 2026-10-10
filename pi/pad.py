#!/usr/bin/env python3
"""The force pad as a joystick, over its USB cable.

    sudo ./pad.py                 # finds the pad, creates the joystick
    sudo ./pad.py /dev/ttyUSB0    # or name the port
    ./pad.py --print              # no joystick, just print the axes (any OS)
    sudo ./pad.py --test          # no pad: joystick fed a 0..1..0 ramp

Why this exists: the pad's board is an ESP32-WROOM-32. Its USB socket is a
serial adapter (CH340), not a USB device of its own, so plugged into the Pi it
shows up as /dev/ttyUSB0 and can never be a joystick by itself. This reads the
axis the pad streams down that serial line and republishes it through uinput
as an ordinary joystick, so the game reads /dev/input/js* (SDL, pygame, jstest)
exactly as it did over Bluetooth:

    axis 0 (X), axis 1 (Y)   0.0 = nothing on the pad, 1.0 = full scale

Which of the two carries the force is set on the pad's page (default: both).
The pad sends exactly 0 at rest - noise and drift are removed in the pad.

It never gives up: unplug the pad, power-cycle it, plug it back - the joystick
stays put (reading 0) and picks up again when the pad answers.

Needs python3-serial and python3-evdev, and write access to /dev/uinput (root).
install_pad.sh does all of it and starts this at every boot - see README.md.
"""
import glob
import hashlib
import subprocess
import sys
import threading
import time
import urllib.request

import serial

NAME = "esp32borne#1"          # what the game sees the joystick called
AXIS_MAX = 32767
ASK_EVERY = 2.0                # s without an axis line before asking again

# The pad's own firmware is kept up to date from here too. firmware/pad.version
# says which version the pad should run; if the pad reports another one, and
# nobody has touched it for a minute, firmware/pad.bin is flashed onto it over
# this same cable. Calibration lives elsewhere in the pad and is not touched.
RAW = "https://raw.githubusercontent.com/dragonsarefake/borne/main/firmware/"
IDLE_BEFORE_FLASH = 60.0       # s of zero force before the pad may be flashed
FLASH_TRIES = 3                # per version, then leave it to a human
target = {"fw": None, "image": None}
tried = {}


class NeedFlash(Exception):
    pass


def watch_target():
    """Background: learn which firmware is published. Never touches the pad,
    so a dead network can never hold up the joystick."""
    wait = 30
    while True:
        time.sleep(wait)
        try:
            with urllib.request.urlopen(RAW + "pad.version", timeout=15) as r:
                fw, sha = r.read().decode().split()
            if fw != target["fw"]:
                # Fetch the image here too, and only announce the version once
                # the image is in hand and matches its checksum. GitHub can
                # serve the two files a few minutes out of step.
                with urllib.request.urlopen(RAW + "pad.bin", timeout=120) as r:
                    image = r.read()
                if image[:1] != b"\xe9" or hashlib.sha256(image).hexdigest() != sha:
                    raise ValueError("image and checksum out of step")
                target["image"], target["fw"] = image, fw
            wait = 3600
        except Exception:
            wait = 300                    # offline: ask again in five minutes


def board_version(line):
    """b'$ID,LOADCELL,fw=1.1.0,proto=1' -> '1.1.0'; anything else -> None."""
    if not line.startswith(b"$ID,"):
        return None
    for part in line.strip().split(b","):
        if part.startswith(b"fw="):
            return part[3:].decode("ascii", "replace")
    return None


def flash_due(board_fw, idle_s):
    fw = target["fw"]
    return bool(fw and board_fw and board_fw != fw
                and tried.get(fw, 0) < FLASH_TRIES and idle_s >= IDLE_BEFORE_FLASH)


def flash(port):
    """Put the published firmware on the pad. Raises if esptool fails."""
    image = target["image"]
    esptool = [sys.executable, "-m", "esptool"]
    if subprocess.run(esptool + ["version"], capture_output=True).returncode:
        subprocess.run(["apt-get", "install", "-y", "esptool"],
                       capture_output=True, timeout=900)
    with open("/tmp/pad.bin", "wb") as f:
        f.write(image)
    done = subprocess.run(esptool + ["--chip", "esp32", "--port", port, "--baud", "460800",
                                     "write_flash", "0x10000", "/tmp/pad.bin"],
                          capture_output=True, timeout=300)
    if done.returncode:
        said = (done.stdout + done.stderr).decode("utf-8", "replace").strip()
        raise RuntimeError("esptool: " + said[-200:])


def find_port():
    for pattern in ("/dev/serial/by-id/*", "/dev/ttyUSB*", "/dev/ttyACM*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def make_joystick():
    from evdev import AbsInfo, UInput, ecodes as e
    axis = AbsInfo(value=0, min=-AXIS_MAX, max=AXIS_MAX, fuzz=0, flat=0, resolution=0)
    # One button, never pressed: udev and SDL decide "joystick" from the
    # presence of a joystick button, and two bare axes alone can be taken for
    # something else.
    ui = UInput({e.EV_ABS: [(e.ABS_X, axis), (e.ABS_Y, axis)],
                 e.EV_KEY: [e.BTN_TRIGGER]}, name=NAME)

    def write(x, y):
        ui.write(e.EV_ABS, e.ABS_X, x)
        ui.write(e.EV_ABS, e.ABS_Y, y)
        ui.syn()
    return write


def parse(line):
    """b'$A,123,456' -> (123, 456); anything else -> None."""
    if not line.startswith(b"$A,"):
        return None
    try:
        x, y = line[3:].strip().split(b",")
        return int(x), int(y)
    except ValueError:
        return None                       # a line cut short by a reset


def run(port, write):
    # Opening the port resets the ESP32 (that is how its USB adapter is
    # wired); it is back within a few seconds and simply gets asked again.
    with serial.Serial(port, 115200, timeout=0.5) as ser:
        asked = 0.0
        heard = time.monotonic()
        pressed = time.monotonic()        # last time the pad sent any force
        board_fw = None
        while True:
            now = time.monotonic()
            if now - heard > ASK_EVERY and now - asked > ASK_EVERY:
                ser.write(b"$AXIS,1\n")   # the pad answers with the current axes
                if board_fw is None:
                    ser.write(b"$ID\n")   # and says which firmware it runs
                asked = now
            # readline(), not "for line in ser": that loop ends silently on the
            # first quiet gap once a timeout is set.
            line = ser.readline()
            board_fw = board_version(line) or board_fw
            xy = parse(line)
            if xy:
                heard = time.monotonic()
                if xy != (0, 0):
                    pressed = heard
                write(*xy)
            if flash_due(board_fw, now - pressed):
                raise NeedFlash()         # leaves the with-block: port closed


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--print" in sys.argv:
        def write(x, y):
            print("X %6.3f   Y %6.3f" % (x / AXIS_MAX, y / AXIS_MAX), flush=True)
    else:
        write = make_joystick()
    if "--test" in sys.argv:
        # No pad needed: both axes ramp 0 -> 1 -> 0 every four seconds, so the
        # joystick and the game's reading of it can be checked on their own.
        print("test pattern on both axes - Ctrl+C to stop", flush=True)
        while True:
            t = time.monotonic() % 4.0
            v = int((t / 2.0 if t < 2.0 else (4.0 - t) / 2.0) * AXIS_MAX)
            write(v, v)
            time.sleep(0.05)
    if "--print" not in sys.argv:
        threading.Thread(target=watch_target, daemon=True).start()
    pending = False                       # a firmware flash is owed
    while True:
        port = args[0] if args else find_port()
        try:
            if not port:
                raise serial.SerialException("no serial port")
            if pending:
                # Also retried when a flash died half way: the pad cannot say
                # which version it runs then, so this does not wait to be asked.
                fw = target["fw"]
                tried[fw] = tried.get(fw, 0) + 1
                pending = tried[fw] < FLASH_TRIES
                write(0, 0)
                try:
                    flash(port)
                    pending = False
                    print("pad firmware updated to %s" % fw, flush=True)
                except Exception as err:
                    print("pad firmware update failed (%s)" % err, file=sys.stderr, flush=True)
                    time.sleep(5)
                    continue
            run(port, write)
        except NeedFlash:
            pending = True
        except (serial.SerialException, OSError) as err:
            write(0, 0)                   # pad gone: let go of the axis
            print("pad not available (%s) - retrying" % err, file=sys.stderr, flush=True)
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
