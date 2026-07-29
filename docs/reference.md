# Gateway Reference

## MQTT configuration schema

File:

- [bridge_config.json](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/spiffs/bridge_config.json)

Schema:

```json
{
  "wifi_ssid": "string",
  "wifi_password": "string",
  "mqtt_uri": "string",
  "mqtt_base_topic": "string"
}
```

## Provisioning AP

If Wi-Fi settings are missing or contain placeholders on first boot, the bridge starts a setup AP:

| Item | Value |
|------|-------|
| SSID | `GW-SETUP-XXXXXX` (last 6 hex digits of the board MAC) |
| Password | `configure123` |
| Gateway IP | `192.168.4.1` |
| Setup page | `http://192.168.4.1/` |
| Sensor status | `http://192.168.4.1/status` |

The setup page has three sections: Wi-Fi/MQTT configuration form, **Mode switch** buttons, and a
**Factory reset** button.

## Live sensor status page

The bridge always runs an HTTP server on port 80 that renders an HTML table of the latest
telemetry from every known Zigbee device. The page **auto-refreshes every 10 seconds**.

| Mode | URL |
|------|-----|
| AP provisioning mode | `http://192.168.4.1/status` |
| Normal STA mode | `http://<device-ip>/` |

Columns: **Device** · **Telemetry** (all fields from the last `trv_telemetry` UART frame) ·
**Last seen** (seconds since last update).

> The device IP in STA mode is printed to serial once the DHCP lease is obtained:
> `Wi-Fi connected — status page: http://x.x.x.x/`

## Factory reset

Two methods are available to clear the saved configuration and return to provisioning AP mode.

### Method 1 — 5 rapid reboots

Power-cycle or hard-reset the board **5 times in a row**, each within **15 seconds** of the
previous one.

Internals:
- An NVS key `bridge/rst_count` is incremented on every boot.
- If the device runs stably for 15 s the counter is reset to 0.
- At count ≥ 5: `bridge_config.json` is deleted and the next boot enters provisioning AP mode.

### Method 2 — Web UI button

Visit `http://192.168.4.1/` (AP mode) or `http://<device-ip>/` (STA mode), scroll to the
**Factory reset** section, and click the red **Factory reset** button. After confirmation the
config is deleted and the device reboots immediately.

### HTTP endpoints for factory reset

`POST /reset` — no body required. Available on both the provisioning server (AP mode) and the
status server (STA mode).

## UART frame types

See:

- [uart_frames.md](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/protocol/uart_frames.md)

Common frame types:

- `trv_telemetry`
- `zigbee_event`
- `mode_status`
- `test_event`
- `test_result`
- `set`
- `mode`
- `permit_join`

## MQTT topics

Base topic: `zigbee2mqtt`

### Telemetry

- `zigbee2mqtt/<device>`

### Commands

- `zigbee2mqtt/<device>/set`

### Gateway control

- `zigbee2mqtt/gateway_control/set`
- `zigbee2mqtt/gateway_control/status`

#### Gateway control payloads

Enable test mode:

```json
{"mode":"test"}
```

Return to normal mode:

```json
{"mode":"normal"}
```

The Wi-Fi bridge receives these on the `gateway_control/set` topic and forwards a `mode` UART
frame to the Zigbee coordinator.  The same command can also be sent from any web page
(`http://192.168.4.1/` in AP mode, or `http://<device-ip>/` in STA mode) without needing an
MQTT connection.

### Test flow

- `zigbee2mqtt/gateway_test/set`
- `zigbee2mqtt/gateway_test/result`

## Device identity

The coordinator emits:

- `friendly_name`
- `display_name`
- `ieee`
- `short_addr`
- `source_ep`

## Zigbee thermostat attributes currently handled

- local temperature
- occupied heating setpoint
- unoccupied heating setpoint
- running state
- system mode
- battery
- battery voltage
- humidity
- pi heating demand
