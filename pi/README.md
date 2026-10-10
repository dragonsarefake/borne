# Raspberry Pi - the force pad as a joystick

The pad plugs into the Pi by USB. Its board's USB socket is a serial adapter,
not a joystick, so one small service on the Pi - `pad.py` - turns the serial
line into an ordinary joystick named `esp32borne#1`. The game just reads a
joystick, through SDL / pygame / jstest or anything else.

The cabinet's buttons are not handled here: they are the game developer's.

## Install

From the Pi's own terminal, with the Pi on the internet - nothing to plug in
and no SD card to pull:

    curl -L github.com/dragonsarefake/borne/raw/main/pi/install_pad.sh | bash

It installs two apt packages, puts `pad.py` in `/opt/pad`, starts it at every
boot, and prints whether the joystick now exists.

    sudo systemctl status pad        # is it running
    journalctl -u pad -n 30          # what it said
    sudo systemctl stop pad && sudo python3 /opt/pad/pad.py --test
                                     # no pad needed: axes ramp 0..1..0

**It has not yet been run on a Pi.**

## It keeps itself up to date

A minute after boot and then hourly, the Pi fetches `pi/pad.py` from this
repository. If it is different and it compiles, it replaces the running one
(the previous is kept as `/opt/pad/pad.py.old`) and the bridge restarts;
otherwise nothing happens. No internet, no problem - the joystick never waits
for the network.

    sudo systemctl start pad-update  # check for an update right now
    journalctl -u pad-update -n 10   # what the last check did
    sudo systemctl disable --now pad-update.timer   # turn auto-update off

## Reading it from the game

    axis 0 (X) and axis 1 (Y)     0.0 = nothing on the pad, 1.0 = full scale

Both axes carry the same force by default; the pad's setup page can restrict
it to one. Full scale is also set there. For example, with pygame:

```python
import pygame
pygame.joystick.init()
pad = pygame.joystick.Joystick(0)   # esp32borne#1
pad.init()

axis = pad.get_axis(0)              # 0.0 empty, 1.0 at full scale
force_kg = axis * FULL_SCALE_KG     # the full scale set on the pad's page
```

What to expect from the axis:

- **Rest is exactly 0.** Sensor noise and slow zero drift are removed in the
  pad, so no deadzone is needed on the game side.
- **The pad does no game logic.** No hit detection, no scoring, no timing: it
  sends the force, and what counts as a hit is the game's decision.
- **It updates about 11 times a second.** A hit lasts 20-80 ms, so it often
  lands on a single sample; reading the axis faster does not add detail.
- If the pad is unplugged the joystick stays, reading 0, and picks up again
  when the pad is back.

## Files

| File | Job |
|---|---|
| `pad.py` | serial line -> joystick. `--print` shows the axes without creating one, `--test` feeds a ramp with no pad |
| `install_pad.sh` | the installer above. Generated - do not edit |
| `make_installer.py` | rebuilds `install_pad.sh` from `pad.py`; run it after changing `pad.py` |
