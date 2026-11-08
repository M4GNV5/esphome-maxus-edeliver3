# ESPHome for the Maxus eDeliver 3 on a MeatPi WiCAN OBD

ESPHome firmware for the [MeatPi WiCAN OBD](https://github.com/meatpiHQ/wican-fw) dongle (ESP32-C3) that
reads a Maxus eDeliver 3 **passively**: it only listens to frames the van broadcasts on the OBD port
and publishes them via MQTT (Home Assistant auto-discovery). It never sends a request or data frame.

| File | Purpose |
|---|---|
| `maxus-edeliver3-passive.yaml` | main config: home WiFi + MQTT, fallback hotspot, CAN log |
| `maxus-edeliver3-charger.yaml` | hotspot-only variant (no home WiFi), e.g. for logging at a charger |
| `secrets.yaml` | dummy values, replace with your own |

## Sensors

| Sensor | Frame | Decoding | Notes |
|---|---|---|---|
| Odometer | `0x540` | bytes 0-2, big endian, km | confirmed against UDS |
| Next service in | `0x540` | bytes 6-7, big endian, km | odometer + countdown = service due km |
| State of charge | `0x6F2` | byte 1, % | **ignition on only** |
| Range | `0x6F0` | byte 0 + upper nibble of byte 1 (12 bit), km | **ignition on only** |
| Outdoor temperature (experimental) | `0x510` | byte 3 − 40, °C | |
| AC charger voltage / current (experimental) | `0x604` | bits 15..2 × 0.1 V / byte 2 × 0.1 A | frame only sent while AC charging |
| Vehicle status (experimental) | | which frames are present | `0x604` = AC charging, `0x606` = DC charging, `0x510` = ignition on |
| Vehicle clock (UTC) | `0x558` | `?? ?? MM DD hh mm ss ??` | |
| VIN (last 8) | `0x5B0` | ASCII | |
| 12V battery voltage | ADC | GPIO4 × 116/16 | measured by the dongle |

SOC and range frames carry unrelated values with the ignition off. Ignition is detected by the presence
of frame `0x510`. Pack voltage/current, tire pressures and the trip odometer were not found on the
OBD port.

## Hardware notes (WiCAN OBD, ESP32-C3)

- CAN TX GPIO0, RX GPIO3, transceiver standby GPIO6 (high = standby)
- LEDs: power GPIO7 (active high), connected GPIO8 / activity GPIO9 (active low)
- The CAN controller must run in **NORMAL** mode. The OBD port is a separate gateway bus without
  other nodes: in LISTENONLY mode nobody ACKs and the gateway resends the same frame (`0x0C9`) forever.
  NORMAL mode only ACKs, nothing is transmitted.

## Flashing

First install: upload `firmware.ota.bin` (from `esphome compile`) in the original WiCAN web interface
(firmware update). The WiCAN partition table has two 1740 KB OTA slots, the image is about 1 MB.
Later updates: OTA upload in the ESPHome web interface or via the ESPHome dashboard.

## CAN logging without WiFi/MQTT

1. Away from home WiFi the dongle opens the hotspot `<name> Fallback AP`
   (the charger variant opens it immediately).
2. Connect a laptop, then turn on logging:
   `curl -X POST 'http://192.168.4.1/switch/CAN%20log/turn_on'`
3. Record until Ctrl-C (`candump -l` format, readable by SavvyCAN / python-can / cantools):

   ```sh
   curl -sN http://192.168.4.1/events | grep --line-buffered -o '([0-9.]*) can0 [0-9A-F#]*' > charge.log
   ```

Lines are dropped silently if WiFi can't keep up. Timestamps are seconds since boot.

## Power

The dongle deep-sleeps for 15 min when there was no CAN traffic for 5 min and the 12 V battery is below
13.2 V. To keep it awake for an OTA update, publish a retained `ON` to `<name>/ota_mode`.
