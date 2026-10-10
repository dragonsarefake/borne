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

The pad reaches the Raspberry Pi over one USB cable, which is both its power
and its data. (The cabinet's buttons are separate and are the game developer's;
nothing here touches them.)

```
   Raspberry Pi 4 --- USB --- ESP32 --- HX711 --- load cell
```

The HX711 is drawn as the green board looks with the chip facing you: `P2`
(six holes) on the left, `P1` (four holes) on the right.

```
  LOAD CELL  LCS-1 S-type              HX711  green board                 ESP32-WROOM-32 devkit
  (5 wires)                      P2                         P1            (USB-C, 30 pins)
                               +------------------------------+
  red     excitation +  -------| E+       [ HX711 ]       GND |---------- GND
  black   excitation -  -------| E-                        DT |---------- GPIO4   (D4)
  white   signal -      -------| A-                       SCK |---------- GPIO5   (D5)
  green   signal +      -------| A+                       VCC |---------- 3V3
                               | B-  empty                    |
                               | B+  empty                    |           USB-C ---------> Raspberry Pi
                               +------------------------------+                            (power + data)
  yellow  shield  ------------- to GND
```

| Wire / pin | Goes to | Note |
|---|---|---|
| cell **red** (excitation +) | HX711 `E+` | |
| cell **black** (excitation -) | HX711 `E-` | |
| cell **white** (signal -) | HX711 `A-` | |
| cell **green** (signal +) | HX711 `A+` | |
| cell **yellow** (shield) | ground | |
| HX711 `GND` | ESP32 `GND` | |
| HX711 `DT` | ESP32 `GPIO4` | data out of the amplifier |
| HX711 `SCK` | ESP32 `GPIO5` | clock into the amplifier |
| HX711 `VCC` | ESP32 `3V3` | not 5 V: the data line must stay at 3.3 V |
| ESP32 USB-C | Raspberry Pi USB | power and data |

The colours are the ones printed on the LCS-1's certificate (red +Exc, black
-Exc, green +Sig, white -Sig, yellow shield). A different cell may differ: go
by its own sheet.

Things that catch people out:

- **`A-` comes before `A+` on the HX711 header**, so white sits above green.
  Wire by the printed label, not by position.
- If the reading goes **down** when you press, the two signal wires are
  swapped. No need to resolder: Setup > Calibration details > Invert the sign.
- Channel B is unused. Leave `B-` and `B+` empty.
- The pins are set at the top of the sketch (`PIN_HX711_DT`, `PIN_HX711_SCK`)
  if you need others. Do not put `SCK` on GPIO 34-39; they are input-only.
- An S-type cell must be loaded **along its axis**, through the threaded ends.
  Side load reads wrong and can damage it.

## What is here

| Path | What |
|---|---|
| `firmware/loadcell_testbed/loadcell_testbed.ino` | the pad's firmware, including its setup web page |
| `firmware/loadcell_testbed/usb_page.py` | open the setup page on a PC through the USB cable |
| `firmware/loadcell_testbed/preview.js`, `page_harness.js`, `extract_page.js` | look at and test the page without a board |
| `pi/pad.py`, `pi/install_pad.sh` | the pad as a joystick on the Pi |

Build: `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/loadcell_testbed`
