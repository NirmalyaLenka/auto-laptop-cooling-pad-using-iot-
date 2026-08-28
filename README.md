# BLE Cooling Pad Controller

An ESP32-based laptop cooling pad that reads the laptop's own CPU
temperature over Bluetooth LE and adjusts fan PWM speed accordingly,
instead of relying on the cooling pad's built-in ambient thermometer
or a fixed-speed switch.

## How it works

1. `firmware/esp32_ble_fan_controller.ino` runs on an ESP32 and acts
   as a BLE GATT server. It advertises as `CoolPad-ESP32`.
2. `host/laptop_temp_ble_client.py` runs on the laptop, reads CPU
   temperature every 2 seconds, and writes it to the ESP32 over BLE.
3. The ESP32 maps the received temperature to a PWM duty cycle and
   drives the cooling pad fan(s) through a MOSFET.
4. If the laptop stops sending updates (script closed, BLE dropped,
   laptop out of range) for more than 15 seconds, the ESP32 falls
   back to a fixed 60% duty cycle rather than stopping the fan.

## Hardware

- ESP32 dev board (any variant with BLE, e.g. ESP32-WROOM-32)
- N-channel logic-level MOSFET, e.g. IRLZ44N or IRLB8721
- 220R resistor (gate), 10k resistor (gate-to-source pulldown)
- 1N4007 flyback diode
- 12V DC fan(s) from the cooling pad, or the pad's existing fan header
- External 12V supply for the fan (do not power the fan from the ESP32)

### Wiring

```
+12V supply ----+---------------------+
                |                     |
              Fan (+)               MOSFET Drain
                |                     |
              Fan (-) ----+---------- (diode cathode to +12V,
                |         |            anode to fan -)
                |         |
                +---------+---- MOSFET Source ---- GND (common)

ESP32 GPIO25 --[220R]-- MOSFET Gate
MOSFET Gate --[10k]-- GND

ESP32 GND ---- MOSFET Source / GND rail (must share ground with the
               12V supply ground)
```

If the cooling pad already has multiple fans wired to a single 12V
rail with one connector, you can usually switch that whole rail
through one MOSFET rather than wiring each fan separately, as long
as the MOSFET and diode are rated for the combined current.

## Firmware setup

1. Open `firmware/esp32_ble_fan_controller.ino` in the Arduino IDE
   or PlatformIO.
2. Install the ESP32 board package if not already installed
   (Board Manager -> esp32 by Espressif Systems).
3. Select your ESP32 board and port, then upload.
4. Open the Serial Monitor at 115200 baud to confirm
   `BLE cooling pad controller ready, advertising as CoolPad-ESP32`.

Adjust `FAN_PWM_PIN`, `TEMP_MIN`, `TEMP_MAX`, `DUTY_MIN_PCT`, and
`FALLBACK_DUTY_PCT` at the top of the file to match your fan and
comfort/noise preference. Defaults below assume a laptop that idles
around 40-50C and can reach the high 70s/low 80s under load; if your
idle or load numbers differ, shift `TEMP_MIN`/`TEMP_MAX` to match.

## Host script setup

```bash
cd host
pip install -r requirements.txt
python laptop_temp_ble_client.py
```

- **Linux**: install `lm-sensors` for reliable CPU temperature
  reporting: `sudo apt install lm-sensors && sudo sensors-detect`.
- **Windows**: install and run
  [OpenHardwareMonitor](https://openhardwaremonitor.org/) (or
  LibreHardwareMonitor) in the background as Administrator before
  starting the script; it exposes sensor data over WMI that the
  script reads from.
- **macOS**: not supported (no public CPU temperature API without a
  third-party kernel extension).

The script auto-reconnects if the BLE link drops, and prints the
ESP32's reported fan duty cycle and link state each second.

## BLE protocol

| Characteristic | UUID                                   | Direction        | Payload                                   |
|-----------------|-----------------------------------------|-------------------|--------------------------------------------|
| Temperature      | `6b1e6a11-6e1a-4a0c-9e21-6f1a9c8a10f0` | laptop -> ESP32   | int16 little-endian, `temp_C * 100`        |
| Status            | `6b1e6a12-6e1a-4a0c-9e21-6f1a9c8a10f0` | ESP32 -> laptop (notify) | 3 bytes: `[temp_rounded, duty_pct, is_fresh]` |

Service UUID: `6b1e6a10-6e1a-4a0c-9e21-6f1a9c8a10f0`

## Fan curve (default)

| Temp        | Duty  |
|-------------|-------|
| <= 48 C     | 20% (covers normal idle) |
| 48-78 C     | linear ramp 20% -> 100% |
| >= 78 C     | 100%  |
| link lost (>15s) | 50% fallback |

A 2C hysteresis band prevents duty cycle from jittering on small
temperature fluctuations.

## Possible extensions

- Add a manual override characteristic (writeable duty% that ignores
  the fan curve) for a "silent mode" button on the pad.
- Add a small OLED on the ESP32 showing current temp/duty for
  visibility without the laptop screen.
- Log GPU temperature as well and drive fan off `max(cpu, gpu)`.
