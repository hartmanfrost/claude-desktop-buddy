# claude-desktop-buddy — Waveshare ESP32-C6-LCD-1.47 fork

A fork of [anthropics/claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
that runs the desk pet firmware on the Waveshare ESP32-C6-LCD-1.47 dev
board, with WiFi station support and over-the-air auto-update from
GitHub Releases.

> Looking for the M5StickC Plus original? See the
> [upstream repository](https://github.com/anthropics/claude-desktop-buddy)
> — its `main` branch tracks the M5StickC build. This fork's `main` follows
> upstream protocol changes but ships a Waveshare-targeted firmware.

## What changed vs. upstream

| Upstream (M5StickC Plus)        | This fork (Waveshare ESP32-C6-LCD-1.47)                      |
| ------------------------------- | ------------------------------------------------------------ |
| ESP32 PICO                      | ESP32-C6 (Wi-Fi 6 + BLE 5)                                   |
| ST7789v2 135×240                | ST7789 172×320 (more vertical breathing room)                |
| MPU6886 IMU                     | none — face-down nap replaced with idle-timer nap            |
| AXP192 PMIC + Li-Po             | USB-only, no battery; brightness via PWM                     |
| BM8563 RTC                      | ESP32-C6 internal clock, time sync from desktop              |
| Buzzer on GPIO2                 | none — beep events flash the WS2812 RGB LED on GPIO8         |
| BtnA + BtnB + Pwr               | single BOOT button (GPIO9), tap/double-tap/hold decoded      |
| WiFi unused                     | WiFi station: connects to strongest saved AP, 3-fail backoff |
| Single app slot                 | OTA: ota_0 / ota_1 ping-pong from GitHub Releases            |
| 18 ASCII species + GIF pet      | unchanged                                                    |

Sources in [`src/`](src/) are the upstream code with minimal targeted
edits plus three new subsystems:
- [`src/wifi_link.cpp`](src/wifi_link.cpp) — non-blocking WiFi station
- [`src/wifi_creds.h`](src/wifi_creds.h) — multi-network storage
- [`src/ota_update.cpp`](src/ota_update.cpp) — GitHub Releases OTA

[`lib/M5Shim/`](lib/M5Shim) is a drop-in `M5StickCPlus.h` that re-exposes
the M5 API on top of LovyanGFX + Arduino primitives. Upstream
`#include <M5StickCPlus.h>` lines compile as-is.

## Single-button UX

Only the BOOT pin is wired to a button — RESET is hard-wired to chip reset.
[`lib/M5Shim/src/M5StickCPlus.cpp`](lib/M5Shim/src/M5StickCPlus.cpp)
synthesizes the three original button actions from one switch:

| Gesture       | Original action        | Use it for            |
| ------------- | ---------------------- | --------------------- |
| Short tap     | `BtnA.wasReleased`     | next screen / approve |
| Hold ≥ 400 ms | `BtnB.wasPressed`      | scroll / page / deny  |
| Double-tap    | `BtnA.pressedFor(600)` | open menu             |

Single-tap is deferred ~350 ms to disambiguate from a double-tap; that's
the only visible delay the rework introduces.

## Building (Docker only — no host toolchain)

```bash
# 1. Firmware build (~15 min first time, ~30 s incremental)
docker compose run --rm build

# 2. LittleFS image with the bundled GIF character
docker compose run --rm fs
```

Output: `.pio/build/waveshare-esp32-c6/firmware.bin` and `littlefs.bin`.

## First flash (USB)

The new OTA-capable partition layout in
[`partitions/ota_8mb.csv`](partitions/ota_8mb.csv) needs a one-time serial
flash. After that all updates ship over the air.

On macOS / Linux with Python and esptool:

```bash
# Backup current flash first (recommended for upgrades)
esptool --chip esp32c6 --port /dev/cu.usbmodem101 --baud 460800 \
        read_flash 0 0x400000 backup.bin

# Wipe otadata so the bootloader starts from ota_0
esptool --chip esp32c6 --port /dev/cu.usbmodem101 --baud 460800 \
        erase_region 0x3F0000 0x2000

# Flash bootloader + partition table + firmware
esptool --chip esp32c6 --port /dev/cu.usbmodem101 --baud 460800 \
        write_flash --flash_mode dio --flash_freq 80m --flash_size 8MB \
        0x0     .pio/build/waveshare-esp32-c6/bootloader.bin \
        0x8000  .pio/build/waveshare-esp32-c6/partitions.bin \
        0x10000 .pio/build/waveshare-esp32-c6/firmware.bin
```

Windows users: [`scripts/flash-from-host.ps1`](scripts/flash-from-host.ps1)
wraps the same logic. There's also `docker compose run --rm flash` which
needs [usbipd-win](https://github.com/dorssel/usbipd-win) to expose COM3
into WSL — see the script comments.

**Important** — flash with `--flash-mode dio`, not `qio`. The arduino-esp32
v3.x bootloader for ESP32-C6 is built DIO; flashing the app in QIO leaves
the chip in a TG0_WDT reset loop right after the ROM prints `ets_loader.c
67`. Both the docker-compose `flash` service and the PowerShell helper
already set this. PlatformIO picks it up from `board_build.flash_mode = dio`.

## WiFi configuration

Three delivery channels — pick whichever fits:

**1. Direct BLE command** ([`scripts/wifi-push-ble.py`](scripts/wifi-push-ble.py)):

```bash
pip install bleak
scripts/wifi-push-ble.py add "MyNetwork" "mypassword"
scripts/wifi-push-ble.py list
scripts/wifi-push-ble.py remove "OldNetwork"
scripts/wifi-push-ble.py clear
```

Same JSON protocol as the desktop app; any BLE client (nRF Connect, Web
Bluetooth) works once paired. See [REFERENCE.md](REFERENCE.md) for the
`cmd:"wifi"` schema.

**2. Folder push via Claude desktop app:**

Create a folder with `wifi.json` inside:

```json
{"nets":[{"ssid":"Home","psk":"a"},{"ssid":"Work","psk":"b"}]}
```

Drag the folder onto the **Hardware Buddy** window (Developer →
Open Hardware Buddy…). The file gets routed to `/config/wifi.json` on
LittleFS without touching the installed character.

**3. USB-Serial** ([`scripts/wifi-push-serial.py`](scripts/wifi-push-serial.py)):

```bash
pip install pyserial
scripts/wifi-push-serial.py add "MyNetwork" "mypassword"
```

The buddy stores up to 8 networks. On boot (and once an hour) it scans,
filters for saved SSIDs, ranks by RSSI, and connects to the strongest. If
the strongest fails three times in a row (wrong password / unreachable),
it falls through to the next. When all candidates exhaust, it backs off
for 5 minutes and rescans.

## OTA auto-update

Once WiFi is up, the device polls `https://api.github.com/repos/OWNER/REPO/releases/latest`
30 seconds after boot and then once an hour. If the release `tag_name`
sorts newer than the compiled-in `FIRMWARE_VERSION`, it downloads the
`firmware-waveshare-c6.bin` asset over HTTPS, writes it to the inactive
OTA partition, and reboots into the new image.

Configured via [`platformio.ini`](platformio.ini) `build_flags`:

```
-DFIRMWARE_VERSION='"0.1.0"'
-DOTA_OWNER='"hartmanfrost"'
-DOTA_REPO='"claude-desktop-buddy"'
-DOTA_ASSET='"firmware-waveshare-c6.bin"'
```

To cut a release: bump `FIRMWARE_VERSION`, build, then

```bash
gh release create v0.2.0 \
   .pio/build/waveshare-esp32-c6/firmware.bin#firmware-waveshare-c6.bin \
   --title "v0.2.0" --notes "What changed…"
```

The `#firmware-waveshare-c6.bin` suffix renames the asset on upload —
must match `OTA_ASSET` exactly.

**Manual trigger.** From any BLE/Serial client: `{"cmd":"ota"}`. Status
visible in the `status` ack under `data.ota`:

```json
{"state":"checking","current":"0.1.0","remote":"0.2.0","progress":0}
```

**Security caveat.** Current build uses `setInsecure()` for the TLS
connection — encryption is on but cert validation is off, so a MITM on
the path to GitHub could substitute firmware. Acceptable on a trusted
home network for hobby use; for anything more, embed the Mozilla CA
bundle via `board_build.embed_files` and switch to `setCACertBundle()`.
There's a `FIXME` comment in [`src/ota_update.cpp`](src/ota_update.cpp).

## Pairing with Claude desktop

Same as upstream — turn on Developer mode (**Help → Troubleshooting →
Enable Developer Mode**) in Claude for macOS/Windows, then
**Developer → Open Hardware Buddy…**, click **Connect**, pick the device
("Claude-XXXX"). Enter the 6-digit passkey shown on the device when
prompted.

## Repo layout

```
src/                   # adapted firmware (small W/H + LED stubs vs upstream,
                       # plus wifi_link, wifi_creds, ota_update)
lib/M5Shim/            # M5StickCPlus.h compatibility layer
characters/            # GIF character pack (bufo, from upstream)
data/characters/       # LittleFS source for `docker compose run --rm fs`
partitions/
  no_ota.csv           # original 4MB single-app layout (for reference)
  ota_8mb.csv          # current 8MB OTA-capable layout
docker/Dockerfile      # PlatformIO + esptool in python:3.12-slim
docker-compose.yml     # build / fs / flash / flashfs / monitor services
scripts/
  attach-usb.ps1       # usbipd helper for docker flashing (Windows)
  flash-from-host.ps1  # host-side esptool flash (Windows)
  wifi-push-ble.py     # push WiFi creds over BLE
  wifi-push-serial.py  # push WiFi creds over USB-Serial
platformio.ini
```
