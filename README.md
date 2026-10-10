# borne - force pad

Joystick `esp32borne#1`. Axis 0 and axis 1: 0.0 at rest, 1.0 at full scale.

## 1. Wire it

HX711 with the chip facing you: `P2` on the left, `P1` on the right.

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

- `A-` is above `A+` on the header: white above green.
- `VCC` goes to `3V3`, not 5 V.
- `B-` and `B+` stay empty.

## 2. Flash the ESP32

PC, ESP32 on USB (replace `COM7` with its port):

    arduino-cli compile --upload -p COM7 --fqbn esp32:esp32:esp32:PartitionScheme=huge_app firmware/loadcell_testbed

## 3. Calibrate

PC, ESP32 on USB:

    python firmware/loadcell_testbed/usb_page.py

Open http://localhost:8712

1. **Setup**: run steps 1 to 3. Step 2 with nothing on the pad, step 3 with a
   known weight on it.
2. **Output to the game**: set the full scale (the force that reads 1.0) and
   the axis (X + Y, X only, Y only).

Same page from a phone: join the hidden wifi `esp32borne#1`, password
`borne123`, open http://192.168.4.1

## 4. Install on the Pi

ESP32 plugged into the Pi by USB, Pi on the internet:

    curl -L github.com/dragonsarefake/borne/raw/main/pi/install_pad.sh | bash

It ends with `OK: the joystick exists`. It starts by itself at every boot and
updates itself from this repository.

## If something is off

| Symptom | Do |
|---|---|
| Reading goes down when pressed | Setup > Calibration details > Invert the sign |
| Axis stuck at 1.0 | full scale is too low: raise it |
| Light press reads 0 | normal: nothing under 1 kg is sent |
| Nothing on the Pi | Debug on the Pi, below |

## Debug on the Pi

Go down the list. The first step that fails is the broken part.

**1. Pad seen on USB?**

    ls /dev/ttyUSB*

Nothing: cable or power. `dmesg | tail -20` after plugging in.

**2. Bridge running?**

    sudo systemctl status pad
    journalctl -u pad -n 30

`pad not available`: back to step 1.

**3. Pad sending?**

    sudo systemctl stop pad
    sudo python3 /opt/pad/pad.py --print

Press the pad: X and Y move. `Ctrl+C`, then `sudo systemctl start pad`.

**4. Joystick there and moving?**

    grep -A4 esp32borne /proc/bus/input/devices
    sudo python3 -m evdev.evtest

Pick `esp32borne#1`, press the pad.

**5. Game ignores it?**

    sudo systemctl stop pad
    sudo python3 /opt/pad/pad.py --test

Both axes ramp 0 to 1 to 0 with no pad. `Ctrl+C`, then `sudo systemctl start pad`.

### Hard-set

Force the serial port:

    sudo systemctl edit pad

Type, save, then `sudo systemctl restart pad`:

    [Service]
    ExecStart=
    ExecStart=/usr/bin/python3 /opt/pad/pad.py /dev/ttyUSB0

Previous bridge version:

    sudo systemctl disable --now pad-update.timer
    sudo cp /opt/pad/pad.py.old /opt/pad/pad.py
    sudo systemctl restart pad

Update now / auto-update back on:

    sudo systemctl start pad-update
    sudo systemctl enable --now pad-update.timer

Start clean: run the install line from step 4 again.

New pad firmware, flashed from the Pi (calibration is kept):

    curl -L github.com/dragonsarefake/borne/raw/main/pi/flash_pad.sh | bash

Talk to the pad (bridge stopped, `Ctrl+]` to exit):

    sudo systemctl stop pad
    python3 -m serial.tools.miniterm /dev/ttyUSB0 115200

| Type | Does |
|---|---|
| `$CFG` | shows calibration and full scale |
| `$SET,fs=50000` | full scale 50 kg (value in grams) |
| `$SET,ax=0` | force on X + Y (`1` X only, `2` Y only) |
