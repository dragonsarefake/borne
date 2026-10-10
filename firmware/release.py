#!/usr/bin/env python3
"""Publish the built firmware so the Pi flashes it onto the pad by itself.

    1. bump FW_VERSION in loadcell_testbed/loadcell_testbed.ino
    2. arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app ^
         --output-dir firmware/loadcell_testbed/build/esp32.esp32.esp32 firmware/loadcell_testbed
    3. python firmware/release.py
    4. commit and push

Writes pad.bin (the app image) and pad.version ("<version> <sha256>").
"""
import hashlib
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INO = os.path.join(HERE, "loadcell_testbed", "loadcell_testbed.ino")
BUILT = os.path.join(HERE, "loadcell_testbed", "build", "esp32.esp32.esp32",
                     "loadcell_testbed.ino.bin")

with open(INO, encoding="utf-8") as f:
    m = re.search(r'FW_VERSION\s*=\s*"([^"]+)"', f.read())
if not m:
    sys.exit("FW_VERSION not found in the sketch")
version = m.group(1)

if not os.path.exists(BUILT):
    sys.exit("no built image - compile first (step 2)")
if os.path.getmtime(BUILT) < os.path.getmtime(INO):
    sys.exit("the built image is OLDER than the sketch - compile again (step 2)")
with open(BUILT, "rb") as f:
    image = f.read()
if image[:1] != b"\xe9":
    sys.exit("the built file is not an ESP32 app image")
if version.encode() not in image:
    sys.exit("version %s is not inside the built image - compile again" % version)

old = ""
try:
    with open(os.path.join(HERE, "pad.version")) as f:
        old = f.read().split()[0]
except (OSError, IndexError):
    pass

sha = hashlib.sha256(image).hexdigest()
shutil.copyfile(BUILT, os.path.join(HERE, "pad.bin"))
with open(os.path.join(HERE, "pad.version"), "w", newline="\n") as f:
    f.write("%s %s\n" % (version, sha))
print("published %s (%d bytes, sha256 %s...)" % (version, len(image), sha[:12]))
if old == version:
    print("WARNING: the version is still %s. A pad already on %s will NOT be"
          " reflashed - bump FW_VERSION if the firmware changed." % (version, version))
