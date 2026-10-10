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

Everything reaches the Raspberry Pi over two USB cables: one from the ESP32
(the force pad) and one from the button board.

```
                                 +--> USB --- ESP32 --- HX711 --- load cell     (force)
   Raspberry Pi 4  --------------+
                                 +--> USB --- EG STARTS board --- 10 buttons    (buttons)
```

### Force pad: load cell -> HX711 -> ESP32

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

### Buttons: EG STARTS board

Each button is one two-pin plug. Which socket a button goes in does not
matter: `pi/scan.py map` asks you to press each one and records it.

```
  +--------------------------------------------------------------------------+
  | [USB cable]   Mode  Clear  Turbo  Auto  |  Up  Down  Right  Left |  5V   |
  |   to the Pi    \_____ leave empty ____/    \___ not used ______/         |
  |                                                                          |
  |                          EG STARTS                         [5-pin stick] |
  |                                                              not used    |
  |  K1   K2   K3   K4   K5   K6   K7   K8   Select  Start   K11   K12       |
  +--------------------------------------------------------------------------+
     \_______________ the 10 game buttons, any 10 of these 12 ______________/
```

- 10 buttons (5 per side of the screen) into any ten of the twelve sockets on
  the bottom row.
- **Leave `Mode`, `Clear`, `Turbo` and `Auto` empty.** They switch on autofire,
  which the game cannot tell from real presses. With nothing plugged in there
  is no way to trigger it.
- The board's USB cable goes to the Pi. Then: `pi/scan.py devices`, `test`,
  `map` - see [pi/README.md](pi/README.md).

## What is here

| Path | What |
|---|---|
| `firmware/loadcell_testbed/loadcell_testbed.ino` | the pad's firmware, including its setup web page |
| `firmware/loadcell_testbed/usb_page.py` | open the setup page on a PC through the USB cable |
| `firmware/loadcell_testbed/preview.js`, `page_harness.js`, `extract_page.js` | look at and test the page without a board |
| `pi/pad.py`, `pi/install_pad.sh` | the pad as a joystick on the Pi |
| `pi/scan.py`, `pi/buttons.py` | the cabinet's USB button board |

Build: `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/loadcell_testbed`
