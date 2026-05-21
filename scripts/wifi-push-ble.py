#!/usr/bin/env python3
"""Push WiFi credentials to a Claude buddy device over BLE (Nordic UART).

Usage:
    wifi-push-ble.py add <ssid> <psk>
    wifi-push-ble.py remove <ssid>
    wifi-push-ble.py list
    wifi-push-ble.py clear
    wifi-push-ble.py bulk <wifi.json>

The script scans for an advertised name starting with "Claude", connects,
writes a JSON command line to the NUS RX characteristic, and prints the
ack notification from TX.

On first run the OS will prompt for the 6-digit passkey shown on the
device. Re-runs reuse the stored bond.

Requires: pip install bleak
"""

import asyncio
import json
import sys
from pathlib import Path

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("Need bleak: pip install bleak", file=sys.stderr)
    sys.exit(1)

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
DEVICE_NAME_PREFIX = "Claude"
SCAN_TIMEOUT = 10.0
RESPONSE_TIMEOUT = 5.0


async def find_device():
    print(f"Scanning for {DEVICE_NAME_PREFIX}* ({SCAN_TIMEOUT:.0f}s)...",
          file=sys.stderr)
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if d.name and d.name.startswith(DEVICE_NAME_PREFIX):
            print(f"Found {d.name} @ {d.address}", file=sys.stderr)
            return d
    print(f"No {DEVICE_NAME_PREFIX}-* device found", file=sys.stderr)
    sys.exit(1)


async def send_cmd(cmd: dict):
    device = await find_device()
    response_buf = bytearray()
    response_event = asyncio.Event()

    def on_notify(_, data: bytearray):
        response_buf.extend(data)
        if b"\n" in response_buf:
            response_event.set()

    async with BleakClient(device) as client:
        await client.start_notify(NUS_TX, on_notify)
        payload = (json.dumps(cmd) + "\n").encode()
        await client.write_gatt_char(NUS_RX, payload, response=False)
        try:
            await asyncio.wait_for(response_event.wait(),
                                   timeout=RESPONSE_TIMEOUT)
        except asyncio.TimeoutError:
            print("No ack within timeout", file=sys.stderr)
            await client.stop_notify(NUS_TX)
            return None
        await client.stop_notify(NUS_TX)

    line = bytes(response_buf).split(b"\n", 1)[0]
    try:
        return json.loads(line)
    except json.JSONDecodeError as exc:
        print(f"Bad ack JSON: {exc}: {line!r}", file=sys.stderr)
        return None


def usage():
    print(__doc__, file=sys.stderr)
    sys.exit(1)


async def main():
    if len(sys.argv) < 2:
        usage()

    op = sys.argv[1]
    cmd = {"cmd": "wifi"}

    if op == "add":
        if len(sys.argv) != 4:
            usage()
        cmd["ssid"] = sys.argv[2]
        cmd["psk"] = sys.argv[3]
    elif op == "remove":
        if len(sys.argv) != 3:
            usage()
        cmd["ssid"] = sys.argv[2]
        cmd["action"] = "remove"
    elif op == "list":
        cmd["action"] = "list"
    elif op == "clear":
        cmd["action"] = "clear"
    elif op == "bulk":
        if len(sys.argv) != 3:
            usage()
        data = json.loads(Path(sys.argv[2]).read_text())
        cmd["nets"] = data["nets"] if "nets" in data else data
    else:
        usage()

    ack = await send_cmd(cmd)
    if ack is None:
        sys.exit(2)
    print(json.dumps(ack, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    asyncio.run(main())
