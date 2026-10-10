# borne - force pad for the arcade cabinet

An ESP32 + HX711 + load cell that the game reads as a joystick axis:
0.0 with nothing on the pad, 1.0 at full scale, on axis 0 and axis 1.

## Install on the Raspberry Pi

The pad plugs into the Pi by USB. With the Pi on the internet, type this in
its terminal:

    curl -L github.com/dragonsarefake/borne/raw/main/pi/install_pad.sh | bash

It installs two packages, starts a small service at every boot, and says
whether the joystick `esp32borne#1` now exists. Details: [pi/README.md](pi/README.md).

## What is here

| Path | What |
|---|---|
| `firmware/loadcell_testbed/loadcell_testbed.ino` | the pad's firmware, including its setup web page |
| `firmware/loadcell_testbed/usb_page.py` | open the setup page on a PC through the USB cable |
| `firmware/loadcell_testbed/preview.js`, `page_harness.js`, `extract_page.js` | look at and test the page without a board |
| `pi/pad.py`, `pi/install_pad.sh` | the pad as a joystick on the Pi |
| `pi/scan.py`, `pi/buttons.py` | the cabinet's USB button board |

Build: `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/loadcell_testbed`
