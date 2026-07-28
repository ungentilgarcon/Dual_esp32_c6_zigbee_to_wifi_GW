# Dual ESP32-C6 Zigbee -> Wi-Fi Gateway (Thermostat Focus)

This repository gives you a **working starter architecture** for a dual ESP32-C6 gateway:

- **Board A (Zigbee side)**: real Zigbee coordinator firmware exposing TRV telemetry and actions over UART JSON lines.
- **Board B (Wi-Fi side)**: MQTT bridge firmware publishing/subscribing with Zigbee2MQTT-compatible topic patterns so systems like **THERMOCALC** can interact in/out.

Start here:

- [Full guide](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/docs/guide.md)
- [Reference sheet](C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/docs/reference.md)

## Why this architecture

For ESP32-C6, Zigbee + Wi-Fi can run on one chip but RX concurrency is limited. Espressif examples recommend a dual-SoC architecture for reliability/performance.

## Similar projects reviewed (inspiration)

- Espressif `esp_zigbee_gateway` (ESP-IDF example): dual-SoC recommendation and gateway foundation.
- Zigbee2MQTT: topic model and integration approach.
- ESP RainMaker Zigbee Gateway example: network steering / provisioning ideas.
- `ESP32-C6-GatewayOS`: modular service/event-bus gateway organization.
- `esp32c6-zigbee-vital-gateway`: split coordinator/bridge with UART+MQTT pattern.
- OpenMQTTGateway: unified gateway design and MQTT-first integration practices.

## Repository layout

- [esp32c6-zigbee-coordinator/](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-zigbee-coordinator/) - Firmware for Board A (UART producer/consumer for Zigbee side).
- [esp32c6-wifi-bridge/](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/) - Firmware for Board B (Wi-Fi + MQTT + UART bridge).
- [protocol/uart_frames.md](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/protocol/uart_frames.md) - Wire protocol between both boards.

## ThermoCalc compatibility

Wi-Fi bridge publishes thermostat telemetry on:

- `zigbee2mqtt/<friendly_name>`

and listens for control commands on:

- `zigbee2mqtt/<friendly_name>/set`

This matches the common Zigbee2MQTT convention used by projects like THERMOCALC.

### Typical TRV payload fields

The bridge forwards fields commonly consumed by thermostat logic:

- `local_temperature`
- `occupied_heating_setpoint`
- `running_state`
- `valve_opening`
- `battery`
- `system_mode` (when present)

## Hardware wiring (UART)

Cross-connect UART between boards:

- Board A TX -> Board B RX
- Board A RX -> Board B TX
- GND -> GND

Default pins are in:

- [coordinator_config.h](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-zigbee-coordinator/main/coordinator_config.h)
- [bridge_config.h](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/main/bridge_config.h)

## Build

For each app:

1. Open ESP-IDF environment.
2. `idf.py set-target esp32c6`
3. `idf.py build`
4. `idf.py -p <PORT> flash monitor`

## Wi-Fi / MQTT configuration

The Wi-Fi bridge reads its runtime settings from a JSON file stored in SPIFFS:

- [esp32c6-wifi-bridge/spiffs/bridge_config.json](/C:/Users/GBAH/Documents/Dual_esp32_c6_zigbee_to_wifi_GW/esp32c6-wifi-bridge/spiffs/bridge_config.json)

Edit these fields:

- `wifi_ssid`
- `wifi_password`
- `mqtt_uri`
- `mqtt_base_topic`

After changing the file, rebuild and reflash the Wi-Fi bridge so the SPIFFS image updates.

If the JSON still contains placeholders on first boot, the bridge starts a setup AP:

- SSID: `GW-SETUP-XXXXXX`
- password: `configure123`

Open `http://192.168.4.1/` to save the Wi-Fi and MQTT settings, then it reboots and joins your network.

## Self-test routine

Use this to check the full UART + MQTT loop before real TRVs are added:

1. Flash the **Wi-Fi bridge** board and connect it to your broker.
2. Flash the **coordinator** board.
3. Wire UART crosswise:
   - coordinator TX -> bridge RX
   - coordinator RX -> bridge TX
   - GND -> GND
4. Watch serial logs on both boards.
5. Send a test command to the bridge MQTT topic:
   - `zigbee2mqtt/gateway_test/set`
   - payload: `{"type":"test","action":"ping"}`

Expected result:
- bridge forwards the command over UART
- coordinator replies with `test_result`
- bridge publishes the reply to `zigbee2mqtt/gateway_test/result`
- coordinator publishes `gateway_control/status` with `normal` on boot

If you see the `test_result` in MQTT, the UART round-trip is working.

## Switching modes

Default mode is **normal**.

Use MQTT to switch the whole setup:

- `zigbee2mqtt/gateway_control/set`
- payload: `{"mode":"test"}`

Switch back with:

- payload: `{"mode":"normal"}`

In **test** mode, the coordinator emits test heartbeats while the real Zigbee stack stays active, and the bridge reports mode changes at:

- `zigbee2mqtt/gateway_control/status`

## Next step for real TRVs

The coordinator already uses the real Zigbee stack now. Next refinements are:

- expand the report mapping for additional TRV attributes
- tune any vendor-specific preset mapping for your exact TRV model
- add a richer device registry if your device names differ from `trv_XXXX`

Current preset tuning includes a TRV701Z-specific profile.

## Legal note

This implementation is original code written for your project, inspired by public architecture patterns from the referenced repositories (not a direct code copy).
