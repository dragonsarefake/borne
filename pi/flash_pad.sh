# Flash the pad's firmware from the Pi it is plugged into.
#     curl -L github.com/dragonsarefake/borne/raw/main/pi/flash_pad.sh | bash
# Calibration and settings are kept. Safe to run again if it stops half way.
sudo bash -s <<'FLASH_PAD'
set -e
BIN=https://raw.githubusercontent.com/dragonsarefake/borne/main/firmware/pad.bin

PORT=$(ls /dev/ttyUSB* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then echo "PROBLEM: no /dev/ttyUSB* - the pad is not plugged in"; exit 1; fi

echo "== esptool"
python3 -m esptool version >/dev/null 2>&1 || { apt-get update -qq; apt-get install -y esptool; }

echo "== firmware"
curl -fsSL "$BIN" -o /tmp/pad.bin
# An ESP32 app image starts with 0xE9 and this one is over a megabyte.
if [ "$(head -c1 /tmp/pad.bin | od -An -tx1 | tr -d ' ')" != "e9" ] || [ "$(stat -c%s /tmp/pad.bin)" -lt 1000000 ]; then
  echo "PROBLEM: the download is not a firmware image - nothing was flashed"; exit 1
fi

echo "== flashing $PORT"
systemctl stop pad 2>/dev/null || true          # it holds the port
trap 'systemctl start pad 2>/dev/null || true' EXIT
python3 -m esptool --chip esp32 --port "$PORT" --baud 460800 write_flash 0x10000 /tmp/pad.bin
rm -f /tmp/pad.bin
echo
echo "OK: flashed. The pad restarts by itself."
FLASH_PAD
