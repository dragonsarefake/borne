# borne - force pad for the arcade cabinet

An ESP32 + HX711 + load cell that the game reads as a joystick axis:
0.0 with nothing on the pad, 1.0 at full scale, on axis 0 and axis 1.

## Install on the Raspberry Pi

The pad plugs into the Pi by USB. With the Pi on the internet, type this in
its terminal:

    curl -L github.com/dragonsarefake/borne/raw/main/pi/install_pad.sh | bash

It installs two packages, starts a small service at every boot, and says
whether the joystick `esp32borne#1` now exists. Details: [pi/README.md](pi/README.md).

## Wiring

Three boards: the load cell, the HX711 amplifier (green breakout), and the
ESP32-WROOM-32 devkit. The ESP32's USB cable goes to the Pi and is both its
power and its data.

```
   LOAD CELL                 HX711 (green breakout)              ESP32-WROOM-32 devkit
   LCS-1 S-type             P2 side            P1 side
  +-----------+           +----------------------------+        +------------------+
  |           | excit. +  | E+                     GND |--------| GND              |
  |           |-----------|                            |        |                  |
  |           | excit. -  | E-                      DT |--------| GPIO4            |
  |           |-----------|                            |        |                  |
  |           | signal -  | A-                     SCK |--------| GPIO5            |
  |           |-----------|                            |        |                  |
  |           | signal +  | A+                     VCC |--------| 3V3              |
  |           |-----------|                            |        |                  |
  |           |           | B-  (not used)             |        |        USB  o----+---> Raspberry Pi
  |           | shield    | B+  (not used)             |        |                  |     (any USB port)
  |           |---> GND   +----------------------------+        +------------------+
  +-----------+
```

| From | To | Note |
|---|---|---|
| HX711 `GND` | ESP32 `GND` | |
| HX711 `DT` | ESP32 `GPIO4` | data out of the amplifier |
| HX711 `SCK` | ESP32 `GPIO5` | clock into the amplifier |
| HX711 `VCC` | ESP32 `3V3` | not 5 V: the data line must stay at 3.3 V |
| cell excitation + / - | HX711 `E+` / `E-` | |
| cell signal + / - | HX711 `A+` / `A-` | |
| cell shield (5th wire) | ground | |
| ESP32 USB | Raspberry Pi USB | power and data |

Things that catch people out:

- **`A-` comes before `A+` on the HX711 header.** Wire by the printed label,
  not by position.
- **Wire colours are not listed here on purpose** - they differ between cells.
  Take them from the sheet supplied with yours. Red = excitation +, black =
  excitation -, green = signal +, white = signal - is common, but check.
- If the reading goes **down** when you press, the two signal wires are
  swapped. No need to resolder: Setup > Calibration details > Invert the sign.
- Channel B is unused. Leave `B-` and `B+` empty.
- The pins are set at the top of the sketch (`PIN_HX711_DT`, `PIN_HX711_SCK`)
  if you need others. Do not put `SCK` on GPIO 34-39; they are input-only.
- An S-type cell must be loaded **along its axis**, through the threaded ends.
  Side load reads wrong and can damage it.
- The cabinet's button board is a separate USB device, plugged into the Pi on
  its own cable.

## What is here

| Path | What |
|---|---|
| `firmware/loadcell_testbed/loadcell_testbed.ino` | the pad's firmware, including its setup web page |
| `firmware/loadcell_testbed/usb_page.py` | open the setup page on a PC through the USB cable |
| `firmware/loadcell_testbed/preview.js`, `page_harness.js`, `extract_page.js` | look at and test the page without a board |
| `pi/pad.py`, `pi/install_pad.sh` | the pad as a joystick on the Pi |
| `pi/scan.py`, `pi/buttons.py` | the cabinet's USB button board |

Build: `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/loadcell_testbed`
