# UART protocol between Zigbee board and Wi-Fi board

Each frame is a single-line JSON object (`\n` terminated, UTF-8).

## Uplink (coordinator -> bridge)

Telemetry/event frame example:

`{"type":"trv_telemetry","friendly_name":"trv_bedroom","ieee":"0x00124b0026aa11bb","temperature":19.8,"occupied_heating_setpoint":21.0,"local_temperature":19.7,"running_state":"heat","valve_opening":54,"battery":87,"timestamp":1722155000}`

Network event example:

`{"type":"network","event":"permit_join","open":true,"seconds":180,"timestamp":1722155001}`

## Downlink (bridge -> coordinator)

Set command (from MQTT `/set`):

`{"type":"set","friendly_name":"trv_bedroom","payload":{"occupied_heating_setpoint":20.5,"system_mode":"heat"}}`

Permit join command:

`{"type":"permit_join","seconds":180}`

## Mapping to MQTT

- Uplink `type=trv_telemetry` -> publish JSON payload to `zigbee2mqtt/<friendly_name>`.
- MQTT command from `zigbee2mqtt/<friendly_name>/set` -> downlink `type=set` frame with same payload.
