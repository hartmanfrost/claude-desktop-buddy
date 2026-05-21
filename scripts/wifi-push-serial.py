#!/usr/bin/env python3
"""Push WiFi credentials to a Claude buddy device over USB-Serial.

Same command surface as wifi-push-ble.py, just over the USB-CDC port
instead of BLE. The device routes both paths through the same JSON
command dispatcher, so behaviour is identical.

Usage:
    wifi-push-serial.py [--port PORT] add <ssid> <psk>
    wifi-push-serial.py [--port PORT] remove <ssid>
    wifi-push-serial.py [--port PORT] list
    wifi-push-serial.py [--port PORT] clear
    wifi-push-serial.py [--port PORT] bulk <wifi.json>

--port defaults to the first matching /dev/cu.usbmodem* (macOS),
/dev/ttyACM* (Linux), or /dev/ttyUSB*.

Requires: pip install pyserial
"""

import argparse
import glob
import json
import sys
from pathlib import Path

try:
    import serial
except ImportError:
    print("Need pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def autodetect_port():
    for pat in ("/dev/cu.usbmodem*", "/dev/ttyACM*", "/dev/ttyUSB*"):
        matches = glob.glob(pat)
        if matches:
            return matches[0]
    return None


def send_cmd(port: str, cmd: dict, timeout: float = 5.0):
    # Match acks by their "ack" field — the device interleaves status
    # logs on the same UART, and other commands might race in too.
    expected_ack = cmd["cmd"]
    with serial.Serial(port, 115200, timeout=timeout) as ser:
        ser.reset_input_buffer()
        ser.write((json.dumps(cmd) + "\n").encode())
        ser.flush()
        for _ in range(50):
            line = ser.readline()
            if not line:
                break
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue
            if data.get("ack") == expected_ack:
                return data
    return None


def main():
    parser = argparse.ArgumentParser(description="Push WiFi creds over USB-Serial")
    parser.add_argument("--port", default=None,
                        help="Serial port (auto-detect if omitted)")
    sub = parser.add_subparsers(dest="op", required=True)

    p = sub.add_parser("add"); p.add_argument("ssid"); p.add_argument("psk")
    p = sub.add_parser("remove"); p.add_argument("ssid")
    sub.add_parser("list")
    sub.add_parser("clear")
    p = sub.add_parser("bulk"); p.add_argument("file")

    args = parser.parse_args()

    port = args.port or autodetect_port()
    if not port:
        print("No serial port found; pass --port", file=sys.stderr)
        sys.exit(1)
    print(f"Using port {port}", file=sys.stderr)

    cmd = {"cmd": "wifi"}
    if args.op == "add":
        cmd["ssid"] = args.ssid; cmd["psk"] = args.psk
    elif args.op == "remove":
        cmd["ssid"] = args.ssid; cmd["action"] = "remove"
    elif args.op == "list":
        cmd["action"] = "list"
    elif args.op == "clear":
        cmd["action"] = "clear"
    elif args.op == "bulk":
        data = json.loads(Path(args.file).read_text())
        cmd["nets"] = data["nets"] if "nets" in data else data

    ack = send_cmd(port, cmd)
    if ack is None:
        print("No ack received", file=sys.stderr)
        sys.exit(2)
    print(json.dumps(ack, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
