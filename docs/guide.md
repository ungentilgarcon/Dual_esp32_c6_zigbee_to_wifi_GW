# Dual ESP32-C6 Zigbee -> Wi-Fi Gateway Guide

## 1. What this project does

This project uses two ESP32-C6 boards:

- **Zigbee coordinator board**: talks to TRVs and other Zigbee devices.
- **Wi-Fi bridge board**: talks to MQTT and exposes Zigbee2MQTT-style topics.

The boards exchange newline-delimited JSON over UART.

## 2. Current behavior

- Zigbee telemetry is published to MQTT.
- MQTT commands are forwarded back to Zigbee.
- First boot can start a setup AP if Wi-Fi/MQTT settings are missing.
- The coordinator has a real Zigbee stack and a TRV701Z preset profile.

## 3. Build prerequisites

- ESP-IDF v5.5.5 or compatible
- Python installed and available to ESP-IDF
- One USB serial adapter per board or one board at a time for flashing

## 4. Build steps

For each app:

1. Open the ESP-IDF shell.
2. Run `idf.py set-target esp32c6`.
3. Run `idf.py build`.
4. Flash with `idf.py -p <PORT> flash monitor`.

Apps:

- [esp32c6-zigbee-coordinator](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-zigbee-coordinator)
- [esp32c6-wifi-bridge](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge)

## 5. Wiring

UART cross-connect:

- coordinator TX -> bridge RX
- coordinator RX -> bridge TX
- GND -> GND

Default pins are documented in:

- [coordinator_config.h](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-zigbee-coordinator/main/coordinator_config.h)
- [bridge_config.h](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/main/bridge_config.h)

## 6. Wi-Fi and MQTT configuration

The bridge reads settings from:

- [bridge_config.json](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/spiffs/bridge_config.json)

Fields:

- `wifi_ssid`
- `wifi_password`
- `mqtt_uri`
- `mqtt_base_topic`

If the JSON still contains placeholders on boot, the bridge starts a provisioning AP.

Provisioning AP:

- SSID: `GW-SETUP-XXXXXX`
- password: `configure123`

Open `http://192.168.4.1/` and save the values.

## 7. MQTT topic model

Telemetry:

- `zigbee2mqtt/<friendly_name>`

Commands:

- `zigbee2mqtt/<friendly_name>/set`

Gateway control:

- `zigbee2mqtt/gateway_control/set`
- `zigbee2mqtt/gateway_control/status`

Test path:

- `zigbee2mqtt/gateway_test/set`
- `zigbee2mqtt/gateway_test/result`

## 8. Typical payloads

Telemetry example:

```json
{"type":"trv_telemetry","friendly_name":"trv_bedroom","temperature":19.8,"occupied_heating_setpoint":21.0,"battery":87}
```

Command example:

```json
{"type":"set","friendly_name":"trv_bedroom","payload":{"occupied_heating_setpoint":20.5,"system_mode":"heat"}}
```

## 9. Runtime modes

- **normal**: real Zigbee operations
- **test**: test heartbeats plus normal Zigbee stack activity

Switch mode with:

```json
{"mode":"test"}
```

or:

```json
{"mode":"normal"}
```

## 10. TRV naming and registry

Default names are based on short address:

- `trv_XXXX`

The coordinator also records manufacturer/model aliases when it can read them, so you can address devices with richer names.

## 11. TRV701Z preset mapping

The coordinator includes a TRV701Z-specific preset profile.

Current mapping:

- `off`, `away`, `holiday` -> off
- `eco`, `auto`, `schedule`, `program` -> auto
- `manual`, `comfort`, `boost`, `heat` -> heat
- `sleep` -> sleep
- `cool`, `fan_only`, `dry` and other supported values are mapped where applicable

## 12. UART protocol summary

See:

- [uart_frames.md](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/protocol/uart_frames.md)

## 13. Troubleshooting

- If Wi-Fi does not connect, check `bridge_config.json`.
- If provisioning AP never appears, verify the JSON file still contains placeholders.
- If MQTT is silent, confirm the broker URI and topic base.
- If Zigbee commands do not land, check UART wiring and endpoint discovery.

## 14. Recommended next improvements

- captive portal with nicer form
- provisioning reset command/button
- per-device aliases stored in NVS
- vendor-specific TRV tuning beyond TRV701Z
