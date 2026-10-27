"""Connect to the "smart board" over BLE (Nordic UART Service), print each
line live and save everything to a timestamped CSV on the Desktop.

Usage:  python smart_board_logger.py
Stop with Ctrl+C.
"""
import asyncio
import csv
import datetime
import os
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "smart board"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # board -> laptop (notify)
HEADER = ["gx", "gy", "gz", "ax", "ay", "az", "timestamp", "pc_time"]


def desktop_path():
    home = os.path.expanduser("~")
    for d in (os.path.join(home, "OneDrive", "Desktop"), os.path.join(home, "Desktop")):
        if os.path.isdir(d):
            return d
    return home


async def main():
    print(f'Scanning for "{DEVICE_NAME}"...')
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15)
    if device is None:
        sys.exit(f'"{DEVICE_NAME}" not found. Is the board powered and not connected to another device?')

    stamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    out_path = os.path.join(desktop_path(), f"smart_board_{stamp}.csv")
    buffer = ""
    count = 0

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(HEADER)

        def on_notify(_, data: bytearray):
            nonlocal buffer, count
            buffer += data.decode(errors="ignore")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                line = line.strip()
                if not line:
                    continue
                print(line)
                fields = line.split(",")
                if len(fields) == 7:
                    writer.writerow(fields + [datetime.datetime.now().isoformat(timespec="milliseconds")])
                    count += 1
                    f.flush()

        async with BleakClient(device) as client:
            print(f"Connected to {device.address}. Saving to {out_path}")
            print(",".join(HEADER[:7]))
            await client.start_notify(NUS_TX_UUID, on_notify)
            try:
                while client.is_connected:
                    await asyncio.sleep(0.5)
                print("Board disconnected.")
            except asyncio.CancelledError:
                pass

    print(f"Saved {count} rows to {out_path}")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
