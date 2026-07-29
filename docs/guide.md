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

### Provisioning AP (fallback)

Whenever the bridge cannot load valid Wi-Fi/MQTT settings — either because `bridge_config.json`
still has placeholder values or is missing entirely — it automatically starts a setup access point
instead of trying to connect to Wi-Fi.

| Item | Value |
|------|-------|
| SSID | `GW-SETUP-XXXXXX` (last 6 hex digits of the board MAC) |
| Password | `configure123` |
| Gateway IP | `192.168.4.1` |
| Config page | `http://192.168.4.1/` |
| Sensor status | `http://192.168.4.1/status` |

Steps:
1. Connect your phone or laptop to the `GW-SETUP-XXXXXX` network with password `configure123`.
2. Open `http://192.168.4.1/` in a browser.
3. Fill in Wi-Fi SSID, Wi-Fi password, MQTT broker URI (e.g. `mqtt://192.168.1.10:1883`) and MQTT topic base (default `zigbee2mqtt`).
4. Click **Save & reboot**. The bridge saves the config to SPIFFS and reboots automatically.

> The AP is only active when the config is invalid.  Once valid credentials are saved, normal
> STA+MQTT operation starts on the next boot.

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

The mode switch is initiated from the **Wi-Fi bridge**. It receives the command (via MQTT or the
provisioning/status web page) and forwards it to the Zigbee coordinator over UART.

### Option A — Web page (no MQTT required)

- **In AP mode** (`http://192.168.4.1/`): use the **Mode switch** buttons at the bottom of the setup page, or visit `http://192.168.4.1/status`.
- **In normal STA mode**: visit `http://<device-ip>/` — the status page always includes mode switch buttons.

### Option B — MQTT publish

During normal operation, publish to:

```
topic:   zigbee2mqtt/gateway_control/set
payload: {"mode":"test"}
```

Switch back with:

```
payload: {"mode":"normal"}
```

#### Via Home Assistant (Developer Tools)

1. Open **Developer Tools → Actions**.
2. Search for `mqtt.publish`.
3. Set:
   - **Topic**: `zigbee2mqtt/gateway_control/set`
   - **Payload**: `{"mode":"test"}`
4. Click **Perform action**.

#### Via MQTT Explorer (desktop GUI, easiest)

1. Download [MQTT Explorer](http://mqtt-explorer.com/) and connect to your broker.
2. In the **Publish** panel set:
   - **Topic**: `zigbee2mqtt/gateway_control/set`
   - **Payload**: `{"mode":"test"}`
3. Click **Publish**.

#### Via command line (mosquitto_pub)

```bash
# Enable test mode
mosquitto_pub -h <broker_ip> -t zigbee2mqtt/gateway_control/set -m '{"mode":"test"}'

# Return to normal mode
mosquitto_pub -h <broker_ip> -t zigbee2mqtt/gateway_control/set -m '{"mode":"normal"}'
```

On Windows (Command Prompt), escape the inner quotes:

```cmd
mosquitto_pub -h <broker_ip> -t zigbee2mqtt/gateway_control/set -m "{\"mode\":\"test\"}"
mosquitto_pub -h <broker_ip> -t zigbee2mqtt/gateway_control/set -m "{\"mode\":\"normal\"}"
```

## 10. Live sensor status web page

The bridge always runs an HTTP server on port 80 showing a table of the latest telemetry
received from all Zigbee devices. The page **auto-refreshes every 10 seconds**.

| Mode | URL |
|------|-----|
| AP provisioning mode | `http://192.168.4.1/status` |
| Normal STA mode | `http://<device-ip>/` (logged to serial on first DHCP lease) |

The table shows one row per device with all reported fields (temperature, battery, setpoint, etc.)
and a "last seen N s ago" column. The page also has **Mode switch** and **Factory reset** buttons.

> **Finding the device IP in STA mode:** watch the serial log at boot; the line
> `Wi-Fi connected — status page: http://x.x.x.x/` is printed once the IP is obtained.
> Alternatively, check your router's DHCP client list.

## 11. Factory reset

If the gateway is misconfigured (wrong Wi-Fi password, bad MQTT URI, etc.) and you can no longer
reach it over the network, you can reset it to factory defaults in two ways:

### Method 1 — 5 rapid reboots

Power-cycle (or hard-reset) the board **5 times in a row**, with each reboot happening within
**15 seconds** of the previous one.

What happens internally:
1. NVS stores a rapid-reboot counter that increments on every boot.
2. After 15 seconds of stable operation the counter is cleared to 0.
3. When the counter reaches 5, the firmware deletes `bridge_config.json` and reboots into
   provisioning AP mode on the next start.

### Method 2 — Factory reset button in the web UI

Both the setup page (`http://192.168.4.1/`) and the status page (`http://<device-ip>/`) have a
red **Factory reset** button. Clicking it and confirming the dialog deletes `bridge_config.json`
and reboots into provisioning AP mode immediately.

## 12. TRV naming and registry

Default names are based on short address:

- `trv_XXXX`

The coordinator also records manufacturer/model aliases when it can read them, so you can address devices with richer names.

## 13. TRV701Z preset mapping

The coordinator includes a TRV701Z-specific preset profile.

Current mapping:

- `off`, `away`, `holiday` -> off
- `eco`, `auto`, `schedule`, `program` -> auto
- `manual`, `comfort`, `boost`, `heat` -> heat
- `sleep` -> sleep
- `cool`, `fan_only`, `dry` and other supported values are mapped where applicable

## 14. UART protocol summary

See:

- [uart_frames.md](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/protocol/uart_frames.md)

## 15. Troubleshooting

- If Wi-Fi does not connect, check `bridge_config.json`.
- If provisioning AP never appears, verify the JSON file still contains placeholders (fields starting with `YOUR_`).
- If MQTT is silent, confirm the broker URI and topic base.
- If Zigbee commands do not land, check UART wiring and endpoint discovery.
- If the status page is blank ("No sensor data received yet"), the Zigbee coordinator has not yet
  sent any telemetry; check UART wiring and that the coordinator is running.
- To recover from a bad configuration, use one of the factory reset methods described in section 11.

## 16. Recommended next improvements

- captive portal with nicer form
- provisioning reset command/button
- per-device aliases stored in NVS
- vendor-specific TRV tuning beyond TRV701Z
