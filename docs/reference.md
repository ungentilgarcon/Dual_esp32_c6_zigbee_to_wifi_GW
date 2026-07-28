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

