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
import sys
import time

import serial

NAME = "esp32borne#1"          # what the game sees the joystick called
AXIS_MAX = 32767
ASK_EVERY = 2.0                # s without an axis line before asking again


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
        while True:
            now = time.monotonic()
            if now - heard > ASK_EVERY and now - asked > ASK_EVERY:
                ser.write(b"$AXIS,1\n")   # the pad answers with the current axes
                asked = now
            # readline(), not "for line in ser": that loop ends silently on the
            # first quiet gap once a timeout is set.
            xy = parse(ser.readline())
            if xy:
                heard = time.monotonic()
                write(*xy)


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
    while True:
        port = args[0] if args else find_port()
        try:
            if not port:
                raise serial.SerialException("no serial port")
            run(port, write)
        except (serial.SerialException, OSError) as err:
            write(0, 0)                   # pad gone: let go of the axis
            print("pad not available (%s) - retrying" % err, file=sys.stderr, flush=True)
            time.sleep(2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
