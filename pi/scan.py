#!/usr/bin/env python3
"""Discover what the USB button board actually reports, and build a mapping.

Nothing here assumes the board's HID layout, key codes or button numbering.
The labels silkscreened on the board (K1..K12, Select, Start) are what the
*board* calls them; Linux will call them something else entirely, and this
finds out what.

    ./scan.py devices          list input devices
    ./scan.py watch [-d PATH]  print every event, with kernel timestamps
    ./scan.py test  [-d PATH]  simultaneous-press / ghosting / autofire check
    ./scan.py map   [-d PATH]  interactively build mapping.json
"""

import argparse
import json
import os
import sys
from collections import defaultdict

try:
    from evdev import InputDevice, ecodes, list_devices
except ImportError:
    sys.exit("python-evdev is missing.  pip install evdev  (see README.md)")

from buttons import LOGICAL_BUTTONS, EXTRA_BUTTONS, MAPPING_PATH

# evdev key value codes
UP, DOWN, REPEAT = 0, 1, 2


def find_devices():
    out = []
    for path in sorted(list_devices()):
        try:
            out.append(InputDevice(path))
        except PermissionError:
            print(f"  {path}: permission denied (are you in the 'dialout' "
                  f"or 'input' group?)", file=sys.stderr)
    return out


def pick_device(explicit):
    if explicit:
        return InputDevice(explicit)
    devices = find_devices()
    if not devices:
        sys.exit("No readable input devices. See README.md (groups, brltty).")
    # Anything that reports key events is a candidate; prefer non-built-in.
    candidates = [d for d in devices if ecodes.EV_KEY in d.capabilities()]
    if not candidates:
        sys.exit("No device reports key events.")
    if len(candidates) == 1:
        return candidates[0]
    print("Several devices report keys. Pick one with -d, or choose now:\n")
    for i, d in enumerate(candidates):
        print(f"  [{i}] {d.path:20} {d.name}")
    choice = input("\nindex> ").strip()
    return candidates[int(choice)]


def key_name(code):
    name = ecodes.bytype[ecodes.EV_KEY].get(code, str(code))
    return name[0] if isinstance(name, (list, tuple)) else name


def cmd_devices(_args):
    for d in find_devices():
        caps = d.capabilities()
        kinds = []
        if ecodes.EV_KEY in caps:
            kinds.append(f"{len(caps[ecodes.EV_KEY])} keys")
        if ecodes.EV_ABS in caps:
            kinds.append(f"{len(caps[ecodes.EV_ABS])} axes")
        print(f"{d.path:20} {d.name!r:40} {', '.join(kinds) or 'no keys/axes'}")
        print(f"{'':20} vendor={d.info.vendor:04x} product={d.info.product:04x}")


def cmd_watch(args):
    dev = pick_device(args.device)
    print(f"Watching {dev.path}  {dev.name!r}")
    print("Press buttons. Ctrl-C to stop.\n")
    t0 = None
    for event in dev.read_loop():
        if event.type != ecodes.EV_KEY:
            continue
        # event.timestamp() is the KERNEL's timestamp, not when Python woke up.
        # This is the number to trust for game timing.
        ts = event.timestamp()
        if t0 is None:
            t0 = ts
        action = {DOWN: "down", UP: "up  ", REPEAT: "rpt "}.get(event.value, "?")
        print(f"{ts - t0:8.3f}s  {action}  code={event.code:<5} "
              f"{key_name(event.code)}")


def cmd_test(args):
    """The two failure modes that would quietly ruin a quicktime game."""
    dev = pick_device(args.device)
    print(f"Testing {dev.path}  {dev.name!r}\n")
    print("1. Press and HOLD as many buttons at once as you can reach.")
    print("2. Release everything.")
    print("3. Press a single button once and leave it alone for 3 seconds.")
    print("\nCtrl-C when done.\n")

    held = set()
    max_held = 0
    max_combo = []
    downs = defaultdict(list)
    repeats = defaultdict(int)

    try:
        for event in dev.read_loop():
            if event.type != ecodes.EV_KEY:
                continue
            name = key_name(event.code)
            if event.value == DOWN:
                held.add(name)
                downs[name].append(event.timestamp())
                if len(held) > max_held:
                    max_held = len(held)
                    max_combo = sorted(held)
                print(f"  down {name:20} ({len(held)} held)")
            elif event.value == UP:
                held.discard(name)
            elif event.value == REPEAT:
                repeats[name] += 1
    except KeyboardInterrupt:
        pass

    print("\n--- results ---")
    print(f"Most buttons held simultaneously: {max_held}")
    if max_combo:
        print(f"  {', '.join(max_combo)}")
    if max_held < 4:
        print("  [CHECK] If you pressed more than this at once, the board is")
        print("          dropping or ghosting simultaneous presses. With five")
        print("          buttons per side that will bite you. Try a different")
        print("          encoder - it is not fixable in software.")

    if repeats:
        print("\nAutorepeat (EV_KEY value 2) seen on:")
        for name, n in sorted(repeats.items()):
            print(f"  {name}: {n}")
        print("  This is normal KERNEL autorepeat for keyboard-style devices.")
        print("  Ignore value==2 in your game code and it is harmless.")

    suspicious = {n: ts for n, ts in downs.items() if len(ts) > 2}
    if suspicious:
        print("\n[WARNING] Repeated DOWN events without releases:")
        for name, stamps in suspicious.items():
            gaps = [b - a for a, b in zip(stamps, stamps[1:])]
            avg = sum(gaps) / len(gaps)
            print(f"  {name}: {len(stamps)} downs, mean gap {avg * 1000:.0f} ms")
        print("  Evenly spaced repeats mean the board's AUTOFIRE/TURBO is on.")
        print("  Cancel it (the manual says Ctl + button) and re-run this test.")
        print("  Autofire injects presses your game cannot tell from real ones.")


def cmd_map(args):
    dev = pick_device(args.device)
    print(f"Mapping {dev.path}  {dev.name!r}")
    print("Press the physical button you want for each name.")
    print("Press an ALREADY-ASSIGNED button to skip the current one.\n")

    mapping = {}
    used = set()
    for logical in LOGICAL_BUTTONS + EXTRA_BUTTONS:
        print(f"  {logical:8} > ", end="", flush=True)
        name = key_name(wait_for_press(dev))
        if name in used:
            print(f"{name} already used - {logical} left unmapped")
            continue
        mapping[name] = logical
        used.add(name)
        print(name)

    with open(MAPPING_PATH, "w") as fh:
        json.dump({"device_name": dev.name, "buttons": mapping}, fh, indent=2)
    print(f"\nWrote {MAPPING_PATH}")
    print("Physical-to-logical mapping lives in that file, not in the code -")
    print("re-run 'map' any time the wiring or the board changes.")


def wait_for_press(dev):
    """Block until one key goes down, then swallow its release."""
    code = None
    for event in dev.read_loop():
        if event.type != ecodes.EV_KEY:
            continue
        if event.value == DOWN and code is None:
            code = event.code
        elif event.value == UP and code == event.code:
            return code


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=["devices", "watch", "test", "map"])
    ap.add_argument("-d", "--device", help="e.g. /dev/input/event5")
    args = ap.parse_args()
    {"devices": cmd_devices, "watch": cmd_watch,
     "test": cmd_test, "map": cmd_map}[args.command](args)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
