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
| No joystick on the Pi | `sudo systemctl status pad` then `journalctl -u pad -n 30` |
| Joystick there, game does not react | `sudo systemctl stop pad && sudo python3 /opt/pad/pad.py --test` (axes ramp 0 to 1 without the pad), then `sudo systemctl start pad` |
| Force an update on the Pi | `sudo systemctl start pad-update` |
| Stop auto-update | `sudo systemctl disable --now pad-update.timer` |
