"""
BLE Cooling Pad - laptop-side client.

Reads the laptop's CPU temperature and writes it to the ESP32 fan
controller over Bluetooth LE every UPDATE_INTERVAL_S seconds.

Requires:
    pip install bleak psutil

Windows temperature reading requires OpenHardwareMonitor (or LibreHardwareMonitor)
running in the background with "Run as Administrator", since Windows does not
expose CPU temperature through a standard public API. See read_temp_windows().

Linux temperature reading uses psutil.sensors_temperatures(), which reads from
the kernel hwmon/thermal subsystem (lm-sensors backing recommended: `sudo
apt install lm-sensors && sudo sensors-detect`).

macOS is not supported by this script (no accessible public sensor API
without third-party kernel extensions).
"""

import asyncio
import platform
import sys
import time

import psutil
from bleak import BleakClient, BleakScanner

SERVICE_UUID = "6b1e6a10-6e1a-4a0c-9e21-6f1a9c8a10f0"
TEMP_CHAR_UUID = "6b1e6a11-6e1a-4a0c-9e21-6f1a9c8a10f0"
STATUS_CHAR_UUID = "6b1e6a12-6e1a-4a0c-9e21-6f1a9c8a10f0"

DEVICE_NAME = "CoolPad-ESP32"
UPDATE_INTERVAL_S = 2.0
SCAN_TIMEOUT_S = 10.0

_wmi_conn = None  # cached WMI connection for Windows path


def read_temp_windows():
    """Read CPU package temperature via OpenHardwareMonitor's WMI namespace.
    Returns None if OpenHardwareMonitor is not running."""
    global _wmi_conn
    try:
        import wmi
    except ImportError:
        print("Missing dependency: pip install wmi", file=sys.stderr)
        return None

    try:
        if _wmi_conn is None:
            _wmi_conn = wmi.WMI(namespace="root/OpenHardwareMonitor")
        sensors = _wmi_conn.Sensor()
        temps = [s.Value for s in sensors
                 if s.SensorType == "Temperature" and "CPU" in s.Name]
        if temps:
            return max(temps)
    except Exception as exc:
        print(f"OpenHardwareMonitor WMI read failed: {exc}", file=sys.stderr)
    return None


def read_temp_linux():
    """Read CPU temperature via psutil / lm-sensors."""
    try:
        temps = psutil.sensors_temperatures()
    except AttributeError:
        return None
    if not temps:
        return None

    for label in ("coretemp", "k10temp", "cpu_thermal", "zenpower"):
        if label in temps and temps[label]:
            return max(t.current for t in temps[label])

    # fall back: first available sensor group
    for entries in temps.values():
        if entries:
            return max(t.current for t in entries)
    return None


def read_cpu_temp():
    system = platform.system()
    if system == "Windows":
        return read_temp_windows()
    if system == "Linux":
        return read_temp_linux()
    print(f"Unsupported platform: {system}", file=sys.stderr)
    return None


def encode_temp(temp_c: float) -> bytes:
    raw = int(round(temp_c * 100))
    raw = max(-32768, min(32767, raw))
    return raw.to_bytes(2, byteorder="little", signed=True)


def on_status_notify(_handle, data: bytearray):
    if len(data) >= 3:
        temp_c, duty_pct, fresh = data[0], data[1], data[2]
        state = "live" if fresh else "fallback"
        print(f"  [device] temp={temp_c}C duty={duty_pct}% state={state}")


async def find_device():
    print(f"Scanning for '{DEVICE_NAME}' (up to {SCAN_TIMEOUT_S:.0f}s)...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == DEVICE_NAME,
        timeout=SCAN_TIMEOUT_S,
    )
    return device


async def run():
    device = await find_device()
    if device is None:
        print(f"Could not find '{DEVICE_NAME}'. Is the ESP32 powered on "
              f"and advertising?", file=sys.stderr)
        return

    print(f"Found {device.name} [{device.address}], connecting...")

    async with BleakClient(device) as client:
        print("Connected.")
        try:
            await client.start_notify(STATUS_CHAR_UUID, on_status_notify)
        except Exception as exc:
            print(f"Status notifications unavailable: {exc}", file=sys.stderr)

        while True:
            if not client.is_connected:
                print("Lost connection.", file=sys.stderr)
                break

            temp_c = read_cpu_temp()
            if temp_c is None:
                print("Could not read CPU temperature, skipping this cycle.",
                      file=sys.stderr)
            else:
                payload = encode_temp(temp_c)
                await client.write_gatt_char(TEMP_CHAR_UUID, payload, response=False)
                print(f"Sent temp={temp_c:.1f}C")

            await asyncio.sleep(UPDATE_INTERVAL_S)


async def run_forever():
    """Reconnect loop so a temporary disconnect doesn't kill the script."""
    while True:
        try:
            await run()
        except Exception as exc:
            print(f"Error: {exc}", file=sys.stderr)
        print("Retrying in 5s...")
        await asyncio.sleep(5)


if __name__ == "__main__":
    try:
        asyncio.run(run_forever())
    except KeyboardInterrupt:
        print("\nStopped.")
