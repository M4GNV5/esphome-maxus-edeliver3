# ESPHome for the Maxus eDeliver 3 on a MeatPi WiCAN OBD

ESPHome firmware for the [MeatPi WiCAN OBD](https://github.com/meatpiHQ/wican-fw) dongle (ESP32-C3) that
reads a Maxus eDeliver 3 and publishes the values via MQTT (Home Assistant auto-discovery).

- The **passive** configs only listen to frames the van broadcasts on the OBD port and never send a
  request or data frame.
- The **UDS** config additionally asks the battery management system for more values
  (read-only requests, see [UDS variant](#uds-variant)).

| File | Purpose |
|---|---|
| `maxus-edeliver3-passive.yaml` | main config: home WiFi + MQTT, fallback hotspot, CAN log |
| `maxus-edeliver3-charger.yaml` | hotspot-only variant (no home WiFi), e.g. for logging at a charger |
| `maxus-edeliver3-uds.yaml` + `maxus_uds.h` | passive config + UDS polling of the BMS (not passive) |
| `maxus-edeliver3-charger-uds.yaml` | hotspot-only variant of the UDS config, e.g. for logging while driving |
| `secrets.yaml` | dummy values, replace with your own |

## Sensors

| Sensor | Frame | Decoding | Notes |
|---|---|---|---|
| Odometer | `0x540` | bytes 0-2, big endian, km | confirmed against UDS |
| Next service in | `0x540` | bytes 6-7, big endian, km | odometer + countdown = service due km |
| State of charge | `0x6F2` | byte 1, % | **ignition on only** |
| Range | `0x6F0` | byte 0 + upper nibble of byte 1 (12 bit), km | **ignition on only** |
| HV battery current | `0x6F2` | bytes 4-5 big endian − 400, A, positive = discharging | **ignition on only**; matches display while DC charging |
| HV battery voltage | `0x6F2` | byte 6 × 2, V | **ignition on only**; matches display while DC charging |
| HV battery power | | voltage × current | |
| Speed | `0x6F0` | byte 4, km/h | |
| Tire pressures FL / FR / RR / RL | `0x5E0` | bytes 0-3 × 2.75 kPa | `FF` = no data; matches display within 0.01 bar |
| Motor speed (experimental) | `0x23C` | bytes 0-1 big endian, rpm (?) | ≈ 62 × km/h |
| Outdoor temperature (experimental) | `0x510` | byte 3 − 40, °C | |
| AC charger voltage / current (experimental) | `0x604` | bits 15..2 × 0.1 V / byte 2 × 0.1 A | frame only sent while AC charging |
| Vehicle status (experimental) | | which frames are present | `0x604` = AC charging, `0x606` = DC charging, `0x510` = ignition on, speed > 0 = driving |
| Vehicle clock (UTC) | `0x558` | `?? ?? MM DD hh mm ss ??` | |
| VIN (last 8) | `0x5B0` | ASCII | |
| 12V battery voltage | ADC | GPIO4 × 116/16 | measured by the dongle |

SOC and range frames carry unrelated values with the ignition off. Ignition is detected by the presence
of frame `0x510`. The trip odometer was not found on the OBD port.

## UDS variant

`maxus-edeliver3-uds.yaml` includes the passive config and additionally reads the BMS via UDS
(ReadDataByIdentifier, read-only). Requests are only sent while the ignition is on or the van is
charging, so the dongle never wakes the van or keeps it awake.

| Sensor | DID | Decoding |
|---|---|---|
| HV battery current (BMS) | `B105` | (raw − 0x8000) × 0.1 A, negative = charging |
| HV battery voltage (BMS) | `B143` | 0.1 V |
| HV battery power (BMS) | | voltage × current |
| State of charge (BMS) | `D117` | 0.1 % |
| Odometer (BMS) | `E101` | UINT24, 0.1 km |
| HV battery temperature max/min/avg | `B110` | 3 × UINT16, 0.1 °C (unverified) |
| HV battery capacity | `B136` | 0.1 Ah (?) |
| Cell voltage min/max/avg/delta | `B142` | 110 × UINT16 mV, 104 used; full list as JSON on `<name>/bms/cell_voltages` |
| BMS DID B132/B134 (raw) | `B132`, `B134` | unknown |

The decoding was checked against values read while AC charging, and the BMS CAN IDs
`0x748` (request) / `0x7C8` (response) were confirmed while driving: current and voltage from UDS
match the passive values from `0x6F2` (e.g. +147.1 A vs +146 A, 329.9 V vs 328 V). Multi-frame
responses (cell voltages, temperatures) haven't been seen in a log yet.

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
