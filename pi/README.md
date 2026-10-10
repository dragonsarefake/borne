# Raspberry Pi — button board layer

Turns the cabinet's USB button board into logical game events. This is **not**
the game — no graphics, no audio, no rounds, no scoring.

```
  USB button board ──evdev──> L1..L5 / R1..R5 / START / SELECT ──> your game
```

**The force pad comes in over its USB cable** (since 2026-10-10; it used to be
Bluetooth). Its board's USB socket is a serial adapter, not a joystick, so one
small service on the Pi — `pad.py` — turns the serial line into an ordinary
joystick named `esp32borne#1`. The game still just reads a joystick axis
through SDL / pygame / jstest: axis 0 and axis 1, 0.0 at rest, 1.0 at full
scale.

Install it from the Pi's own terminal, with the Pi on the internet — nothing to
plug in, no SD card to pull: **paste the whole of `install_pad.sh` into the
terminal** (or copy the file over and run `bash install_pad.sh`). It installs
two apt packages, puts `pad.py` in `/opt/pad`, and starts it at every boot. It
prints whether the joystick now exists.

    sudo systemctl status pad        # is it running
    journalctl -u pad -n 30          # what it said
    sudo systemctl stop pad && sudo python3 /opt/pad/pad.py --test
                                     # no pad needed: axes ramp 0..1..0

**It keeps itself up to date.** A minute after boot and then hourly, the Pi
fetches `pi/pad.py` from this repository. If it is different and it compiles,
it replaces the running one (the previous is kept as `/opt/pad/pad.py.old`) and
the bridge restarts; otherwise nothing happens. No internet, no problem - the
joystick never waits for the network.

    sudo systemctl start pad-update  # check for an update right now
    journalctl -u pad-update -n 10   # what the last check did
    sudo systemctl disable --now pad-update.timer   # turn auto-update off

`install_pad.sh` is generated from `pad.py` by `make_installer.py`.
**It has not yet been run on a Pi.**

| File | Job |
|---|---|
| `scan.py` | discover the button board, test it, generate `mapping.json` |
| `buttons.py` | board → `L1..L5`, `R1..R5`, `START`, `SELECT` |

One generated file, produced by measurement rather than assumption:
`mapping.json`, which says which physical key is which logical button.

## Install

```bash
sudo apt install python3-pip
pip3 install -r requirements.txt        # evdev
sudo usermod -aG input $USER            # then LOG OUT AND BACK IN
```

Group membership is established at login — a new terminal is not enough. Check
with `groups | grep input`.

## First run, in order

### 1. What is the board?

```bash
./scan.py devices
```

Shows every input device with its name, vendor/product IDs, and how many keys
and axes it reports. The board may appear as a gamepad or as a keyboard — you
need to know which, and nothing here assumes either.

### 2. Does it behave?

```bash
./scan.py test
```

**This step is not optional, and it has never been run.** It checks the two
things that would quietly ruin a quicktime game:

- **Simultaneous presses.** Hold as many buttons as you can reach. With five per
  side, overlapping presses are guaranteed, and cheap encoders drop or ghost
  them. If the reported maximum is lower than what you actually pressed, that is
  a hardware limit — a different encoder, not a software fix.
- **Autofire.** The "EG STARTS" board's manual documents `Auto + button` and
  `Turbo + button` to enable repeat fire, `Ctl + button` to cancel. If it is on,
  the board injects presses your game cannot distinguish from real ones. The
  test flags evenly spaced repeated DOWN events, which is the signature. If it
  fires, clear it with `Ctl + button` and re-run before mapping.

It also distinguishes board autofire from ordinary **kernel autorepeat**
(`EV_KEY` value 2), which keyboard-style devices produce for any held key and is
harmless — `buttons.py` ignores value 2 entirely.

If you are not using the Mode/Turbo/Auto inputs, leave those 2-pin headers
unpopulated. You have 12 button inputs and need 10, so nothing is lost and there
is then no physical way to trigger autofire mid-game.

### 3. Build the mapping

```bash
./scan.py map
```

Prompts for each logical name in turn and records whichever physical key you
press. Writes `mapping.json`:

```json
{
  "device_name": "USB Gamepad",
  "buttons": { "BTN_SOUTH": "L1", "BTN_EAST": "L2", "...": "..." }
}
```

The mapping lives in that file, never in the code. Re-run it whenever the wiring
or the board changes — which is the point: the board's silkscreen labels
(`K1..K12`) never appear above `buttons.py`.

## Row heights

The buttons are physical and sit off-screen, beside the display, so the only
thing the game needs from the cabinet is how high each row is. That now lives on
the **force pad's web page**: measure the cabinet with a tape, type the screen
height and the top and bottom row centres into Settings > Button rows, and the
box puts the five interpolated fractions into its handoff document.

Only the top and bottom are measured; the three between are interpolated. The
margins above row 1 and below row 5 are not the same, which is exactly why both
ends get measured rather than derived from a formula.

The fractions are of screen height, so `y = height * screen_height` works at any
resolution.

(`rowcal.py`, which used to measure these on a screen with the buttons, is gone:
a tape measure and two numbers do the same job without needing the cabinet
assembled and a display attached.)

## Two things this layer gets right on purpose

**Timestamps come from the kernel, not from Python.** Buttons use
`event.timestamp()`, which is accurate even if Python was busy elsewhere.

**The device is grabbed by default.** `buttons.py` calls `device.grab()`, so
presses do not also reach the desktop. Without it, a board emulating a keyboard
types into whatever window has focus while you play. `--no-grab` disables it if
you need the keys elsewhere while debugging.

## Using it from the game

```python
from buttons import Buttons

buttons = Buttons()
buttons.on_press = lambda name, when: ...      # "L3", host time
```

The force axis comes from the gamepad, separately — e.g. with pygame:

```python
import pygame
pygame.joystick.init()
pad = pygame.joystick.Joystick(0)   # esp32borne#1
pad.init()

axis = pad.get_axis(0)              # 0.0 empty, 1.0 at full scale
force_kg = axis * 1000              # full scale defaults to the cell's 1000 kg
```

**Scoring is deliberately not done anywhere below the game.** Closest peak,
fastest to band, longest held, overshoot penalty — that is game design, and it
belongs on the Pi where you can iterate without reflashing an ESP32. The pad
reports facts; the page's Impact readout shows exactly which facts.

## Choosing target forces

Read these off the **measured** figures from the pad's setup, not from a
datasheet — step 1 reports the noise and step 3 reports counts per gram and the
usable resolution. The LCS-1 is rated 0.03 % FS combined error, but the HX711
supplies ~4.3 V against the cell's 10–15 V spec, so the real limit is noise, and
noise is what the box measures for you.

Two things worth knowing before picking a tolerance band:

- Do not set a band tighter than the usable resolution the box reports. A band
  narrower than the noise scores sensor error rather than player skill, and
  players experience that as the game being arbitrary.
- The mechanic is a **hit**, not a press. At 10 SPS the HX711 catches one sample
  per hit or none, so trust **impulse** (area) over **peak** (height). The
  Impact readout flags fewer than 3 samples. Lifting the HX711's RATE pin to VCC
  gives 80 SPS and 2–6 samples per hit, at the cost of more noise; the firmware
  measures the real rate and adapts either way.

## Status

`scan.py` and `buttons.py` are syntax-checked only. **Neither has been run** —
the button board has not yet been connected to anything. Expect to fix something
on first contact, and do step 2 before trusting the board at all.
