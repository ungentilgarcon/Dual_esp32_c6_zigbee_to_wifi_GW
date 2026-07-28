#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "cJSON.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "coordinator_config.h"
#include "zigbee_gateway.h"

static const char *TAG = "ZB_COORD";
static volatile int s_test_mode = 0;

typedef struct {
    bool in_use;
    char friendly_name[24];
    char alias_name[80];
    char manufacturer[32];
    char model[32];
    uint16_t short_addr;
    uint8_t ep;
} coord_device_t;

static coord_device_t s_devices[16];

static void config_report_temperature(void);
static void config_report_battery(void);
static ezb_err_t subscribe_remote_cluster(uint16_t dst_short_addr, uint8_t dst_ep, uint16_t cluster_id);
static ezb_err_t bind_remote_cluster_async(uint16_t dst_short_addr, uint8_t dst_ep, uint16_t cluster_id);
static void zdo_bind_discovery_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx);
static void discover_device(uint16_t short_addr);
static bool parse_trv_short_addr(const char *friendly_name, uint16_t *short_addr);
static coord_device_t *registry_lookup(const char *friendly_name);
static void registry_store(uint16_t short_addr, uint8_t ep, const char *manufacturer, const char *model);
static ezb_err_t write_thermostat_setpoint(uint16_t short_addr, uint8_t ep, float setpoint_c);
static ezb_err_t write_thermostat_system_mode(uint16_t short_addr, uint8_t ep, uint8_t system_mode);
static ezb_err_t write_thermostat_preset_for_device(const char *model, uint16_t short_addr, uint8_t ep, const char *preset);
static void sanitize_name_component(const char *src, char *dst, size_t dst_size);
static const char *thermostat_system_mode_name(uint8_t mode);

static void uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = COORD_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(COORD_UART_PORT, COORD_UART_BUF_SIZE, COORD_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(COORD_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(COORD_UART_PORT, COORD_UART_TX_PIN, COORD_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static bool parse_trv_short_addr(const char *friendly_name, uint16_t *short_addr)
{
    unsigned long value;
    char *endptr;

    if (friendly_name == NULL || short_addr == NULL) {
        return false;
    }
    if (strncmp(friendly_name, "trv_", 4) != 0) {
        return false;
    }

    value = strtoul(friendly_name + 4, &endptr, 16);
    if (endptr == friendly_name + 4 || *endptr != '\0' || value > 0xFFFFUL) {
        return false;
    }

    *short_addr = (uint16_t)value;
    return true;
}

static coord_device_t *registry_lookup(const char *friendly_name)
{
    size_t i;
    for (i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (!s_devices[i].in_use) {
            continue;
        }
        if (strcmp(s_devices[i].friendly_name, friendly_name) == 0 ||
            (s_devices[i].alias_name[0] != '\0' && strcmp(s_devices[i].alias_name, friendly_name) == 0) ||
            (s_devices[i].manufacturer[0] != '\0' && strcmp(s_devices[i].manufacturer, friendly_name) == 0) ||
            (s_devices[i].model[0] != '\0' && strcmp(s_devices[i].model, friendly_name) == 0)) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static void registry_store(uint16_t short_addr, uint8_t ep, const char *manufacturer, const char *model)
{
    char friendly_name[24];
    char alias_name[80];
    char manufacturer_name[32];
    char model_name[32];
    size_t i;

    snprintf(friendly_name, sizeof(friendly_name), "trv_%04hx", short_addr);
    sanitize_name_component(manufacturer, manufacturer_name, sizeof(manufacturer_name));
    sanitize_name_component(model, model_name, sizeof(model_name));
    if (manufacturer_name[0] != '\0' && model_name[0] != '\0') {
        snprintf(alias_name, sizeof(alias_name), "%s_%s_%04hx", manufacturer_name, model_name, short_addr);
    } else if (model_name[0] != '\0') {
        snprintf(alias_name, sizeof(alias_name), "%s_%04hx", model_name, short_addr);
    } else if (manufacturer_name[0] != '\0') {
        snprintf(alias_name, sizeof(alias_name), "%s_%04hx", manufacturer_name, short_addr);
    } else {
        alias_name[0] = '\0';
    }

    for (i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (s_devices[i].in_use && s_devices[i].short_addr == short_addr && s_devices[i].ep == ep) {
            snprintf(s_devices[i].friendly_name, sizeof(s_devices[i].friendly_name), "%s", friendly_name);
            if (alias_name[0] != '\0') {
                snprintf(s_devices[i].alias_name, sizeof(s_devices[i].alias_name), "%s", alias_name);
            }
            if (manufacturer_name[0] != '\0') {
                snprintf(s_devices[i].manufacturer, sizeof(s_devices[i].manufacturer), "%s", manufacturer_name);
            }
            if (model_name[0] != '\0') {
                snprintf(s_devices[i].model, sizeof(s_devices[i].model), "%s", model_name);
            }
            return;
        }
    }

    for (i = 0; i < sizeof(s_devices) / sizeof(s_devices[0]); i++) {
        if (!s_devices[i].in_use) {
            s_devices[i].in_use = true;
            s_devices[i].short_addr = short_addr;
            s_devices[i].ep = ep;
            snprintf(s_devices[i].friendly_name, sizeof(s_devices[i].friendly_name), "%s", friendly_name);
            if (alias_name[0] != '\0') {
                snprintf(s_devices[i].alias_name, sizeof(s_devices[i].alias_name), "%s", alias_name);
            }
            if (manufacturer_name[0] != '\0') {
                snprintf(s_devices[i].manufacturer, sizeof(s_devices[i].manufacturer), "%s", manufacturer_name);
            }
            if (model_name[0] != '\0') {
                snprintf(s_devices[i].model, sizeof(s_devices[i].model), "%s", model_name);
            }
            return;
        }
    }
}

static void uart_send_json(cJSON *root)
{
    char *encoded = cJSON_PrintUnformatted(root);
    if (encoded == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return;
    }
    uart_write_bytes(COORD_UART_PORT, encoded, strlen(encoded));
    uart_write_bytes(COORD_UART_PORT, "\n", 1);
    cJSON_free(encoded);
}

static void publish_mode_status(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mode_status");
    cJSON_AddStringToObject(root, "friendly_name", "gateway_control");
    cJSON_AddStringToObject(root, "mode", s_test_mode ? "test" : "normal");
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000));
    uart_send_json(root);
    cJSON_Delete(root);
}

static void publish_test_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "test_event");
    cJSON_AddStringToObject(root, "friendly_name", "gateway_test");
    cJSON_AddStringToObject(root, "board", "coordinator");
    cJSON_AddStringToObject(root, "status", "alive");
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000));
    uart_send_json(root);
    cJSON_Delete(root);
}

static void send_test_result(const char *friendly_name, const char *status, const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "test_result");
    cJSON_AddStringToObject(root, "friendly_name", friendly_name);
    cJSON_AddStringToObject(root, "status", status);
    cJSON_AddStringToObject(root, "detail", detail);
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000));
    uart_send_json(root);
    cJSON_Delete(root);
}

static void publish_trv_field(uint16_t short_addr, uint8_t src_ep, const char *field, double value, const char *state)
{
    char friendly_name[24];
    char display_name[80];
    char ieee_text[32];
    ezb_extaddr_t extaddr = {0};
    coord_device_t *device;

    snprintf(friendly_name, sizeof(friendly_name), "trv_%04hx", short_addr);
    display_name[0] = '\0';
    device = registry_lookup(friendly_name);
    if (device != NULL && device->alias_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s", device->alias_name);
    } else {
        snprintf(display_name, sizeof(display_name), "%s", friendly_name);
    }
    if (ezb_address_extended_by_short(short_addr, &extaddr) == EZB_ERR_NONE) {
        snprintf(ieee_text, sizeof(ieee_text), "0x%016llx", extaddr.u64);
    } else {
        snprintf(ieee_text, sizeof(ieee_text), "0x%04hx", short_addr);
    }

    registry_store(short_addr, src_ep, NULL, NULL);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "trv_telemetry");
    cJSON_AddStringToObject(root, "friendly_name", friendly_name);
    cJSON_AddStringToObject(root, "display_name", display_name);
    cJSON_AddStringToObject(root, "ieee", ieee_text);
    cJSON_AddNumberToObject(root, "source_ep", src_ep);
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000));

    if (strcmp(field, "local_temperature") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "temperature", value);
    } else if (strcmp(field, "occupied_heating_setpoint") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "occupied_heating_setpoint", value);
    } else if (strcmp(field, "battery") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "battery", value);
    } else if (strcmp(field, "battery_voltage") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "battery_voltage", value);
    } else if (strcmp(field, "humidity") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "humidity", value);
    } else if (strcmp(field, "pi_heating_demand") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "pi_heating_demand", value);
    } else if (strcmp(field, "unoccupied_heating_setpoint") == 0) {
        cJSON_AddNumberToObject(root, field, value);
        cJSON_AddNumberToObject(root, "unoccupied_heating_setpoint", value);
    } else if (strcmp(field, "system_mode") == 0 && state != NULL) {
        cJSON_AddNumberToObject(root, "system_mode_raw", value);
        cJSON_AddStringToObject(root, "system_mode", state);
    } else if (strcmp(field, "running_state") == 0 && state != NULL) {
        cJSON_AddNumberToObject(root, "running_state_raw", value);
        cJSON_AddStringToObject(root, "running_state", state);
    } else {
        cJSON_AddNumberToObject(root, field, value);
    }

    uart_send_json(root);
    cJSON_Delete(root);
}

static void publish_discovery_event(uint16_t short_addr, uint8_t src_ep, const char *manufacturer, const char *model)
{
    char friendly_name[24];
    char display_name[80];
    char manufacturer_name[32];
    char model_name[32];
    snprintf(friendly_name, sizeof(friendly_name), "trv_%04hx", short_addr);
    sanitize_name_component(manufacturer, manufacturer_name, sizeof(manufacturer_name));
    sanitize_name_component(model, model_name, sizeof(model_name));
    if (manufacturer_name[0] != '\0' && model_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s_%s_%04hx", manufacturer_name, model_name, short_addr);
    } else if (model_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s_%04hx", model_name, short_addr);
    } else if (manufacturer_name[0] != '\0') {
        snprintf(display_name, sizeof(display_name), "%s_%04hx", manufacturer_name, short_addr);
    } else {
        snprintf(display_name, sizeof(display_name), "%s", friendly_name);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "zigbee_event");
    cJSON_AddStringToObject(root, "friendly_name", friendly_name);
    cJSON_AddStringToObject(root, "display_name", display_name);
    cJSON_AddStringToObject(root, "event", "device_discovered");
    cJSON_AddNumberToObject(root, "short_addr", short_addr);
    cJSON_AddNumberToObject(root, "source_ep", src_ep);
    if (manufacturer != NULL) {
        cJSON_AddStringToObject(root, "manufacturer", manufacturer);
    }
    if (model != NULL) {
        cJSON_AddStringToObject(root, "model", model);
    }
    cJSON_AddNumberToObject(root, "timestamp", (double)(esp_timer_get_time() / 1000000));
    uart_send_json(root);
    cJSON_Delete(root);
    registry_store(short_addr, src_ep, manufacturer, model);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_test_mode) {
            publish_test_heartbeat();
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void handle_set_command(const cJSON *json)
{
    const cJSON *friendly_name = cJSON_GetObjectItemCaseSensitive(json, "friendly_name");
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(json, "payload");
    if (!cJSON_IsString(friendly_name) || !cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "Invalid set command");
        return;
    }

    char *payload_text = cJSON_PrintUnformatted(payload);
    if (payload_text == NULL) {
        ESP_LOGW(TAG, "Failed to encode payload");
        return;
    }

    ESP_LOGI(TAG, "Set command for %s: %s", friendly_name->valuestring, payload_text);
    cJSON_free(payload_text);

    {
        const cJSON *inner_type = cJSON_GetObjectItemCaseSensitive(payload, "type");
        const cJSON *action = cJSON_GetObjectItemCaseSensitive(payload, "action");
        if (cJSON_IsString(inner_type) && inner_type->valuestring != NULL && strcmp(inner_type->valuestring, "test") == 0) {
            const char *status = (cJSON_IsString(action) && action->valuestring != NULL && strcmp(action->valuestring, "ping") == 0) ? "ok" : "ok";
            send_test_result(friendly_name->valuestring, status, "uart-roundtrip-ok");
            return;
        }
    }

    {
        coord_device_t *device = registry_lookup(friendly_name->valuestring);
        uint16_t short_addr = 0;
        uint8_t ep = COORD_ZB_HA_EP_ID;

        if (device != NULL) {
            short_addr = device->short_addr;
            ep = device->ep;
        } else if (!parse_trv_short_addr(friendly_name->valuestring, &short_addr)) {
            ESP_LOGW(TAG, "Unknown device name %s", friendly_name->valuestring);
            return;
        }

        if (cJSON_HasObjectItem(payload, "occupied_heating_setpoint")) {
            const cJSON *setpoint = cJSON_GetObjectItemCaseSensitive(payload, "occupied_heating_setpoint");
            if (cJSON_IsNumber(setpoint)) {
                ezb_err_t ret = write_thermostat_setpoint(short_addr, ep, (float)setpoint->valuedouble);
                ESP_LOGI(TAG, "Write setpoint %.2f to 0x%04hx/ep%u -> 0x%04x", setpoint->valuedouble, short_addr, ep, ret);
            }
        }

        if (cJSON_HasObjectItem(payload, "system_mode")) {
            const cJSON *system_mode = cJSON_GetObjectItemCaseSensitive(payload, "system_mode");
            if (cJSON_IsString(system_mode) && system_mode->valuestring != NULL) {
                uint8_t mode = 0x04;
                if (strcmp(system_mode->valuestring, "off") == 0) {
                    mode = 0x00;
                } else if (strcmp(system_mode->valuestring, "auto") == 0) {
                    mode = 0x01;
                } else if (strcmp(system_mode->valuestring, "cool") == 0) {
                    mode = 0x03;
                } else if (strcmp(system_mode->valuestring, "heat") == 0) {
                    mode = 0x04;
                } else if (strcmp(system_mode->valuestring, "emergency_heating") == 0) {
                    mode = 0x05;
                } else if (strcmp(system_mode->valuestring, "precooling") == 0) {
                    mode = 0x06;
                } else if (strcmp(system_mode->valuestring, "fan_only") == 0) {
                    mode = 0x07;
                } else if (strcmp(system_mode->valuestring, "dry") == 0) {
                    mode = 0x08;
                } else if (strcmp(system_mode->valuestring, "sleep") == 0) {
                    mode = 0x09;
                } else {
                    ESP_LOGW(TAG, "Unsupported system_mode %s", system_mode->valuestring);
                    mode = 0xFF;
                }
                if (mode != 0xFF) {
                    ezb_err_t ret = write_thermostat_system_mode(short_addr, ep, mode);
                    ESP_LOGI(TAG, "Write system_mode %s to 0x%04hx/ep%u -> 0x%04x", system_mode->valuestring, short_addr, ep, ret);
                }
            }
        }

        if (cJSON_HasObjectItem(payload, "preset")) {
            const cJSON *preset = cJSON_GetObjectItemCaseSensitive(payload, "preset");
            if (cJSON_IsString(preset) && preset->valuestring != NULL) {
                ezb_err_t ret = write_thermostat_preset_for_device(device != NULL ? device->model : NULL, short_addr, ep, preset->valuestring);
                ESP_LOGI(TAG, "Write preset %s to 0x%04hx/ep%u -> 0x%04x", preset->valuestring, short_addr, ep, ret);
            }
        }
    }
}

static void handle_mode_command(const cJSON *json)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
        ESP_LOGW(TAG, "Invalid mode command");
        return;
    }

    if (strcmp(mode->valuestring, "test") == 0) {
        s_test_mode = 1;
        ESP_LOGI(TAG, "Switched to TEST mode");
    } else if (strcmp(mode->valuestring, "normal") == 0) {
        s_test_mode = 0;
        ESP_LOGI(TAG, "Switched to NORMAL mode");
    } else {
        ESP_LOGW(TAG, "Unknown mode: %s", mode->valuestring);
        return;
    }

    publish_mode_status();
}

static void handle_permit_join_command(const cJSON *json)
{
    const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(json, "seconds");
    if (!cJSON_IsNumber(seconds)) {
        ESP_LOGW(TAG, "Invalid permit_join command");
        return;
    }

    ESP_LOGI(TAG, "Permit join for %d seconds", seconds->valueint);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_open_network((uint8_t)seconds->valueint);
    esp_zigbee_lock_release();

    cJSON *event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "type", "network");
    cJSON_AddStringToObject(event, "event", "permit_join");
    cJSON_AddBoolToObject(event, "open", 1);
    cJSON_AddNumberToObject(event, "seconds", seconds->valueint);
    cJSON_AddNumberToObject(event, "timestamp", (double)(esp_timer_get_time() / 1000000));
    uart_send_json(event);
    cJSON_Delete(event);
}

static void command_task(void *arg)
{
    (void)arg;
    uint8_t buf[COORD_UART_BUF_SIZE];
    int pos = 0;
    memset(buf, 0, sizeof(buf));

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(COORD_UART_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos == 0) {
                continue;
            }
            buf[pos] = '\0';
            cJSON *root = cJSON_Parse((const char *)buf);
            if (root != NULL) {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
                if (cJSON_IsString(type) && type->valuestring != NULL) {
                    if (strcmp(type->valuestring, "set") == 0) {
                        handle_set_command(root);
                    } else if (strcmp(type->valuestring, "mode") == 0) {
                        handle_mode_command(root);
                    } else if (strcmp(type->valuestring, "permit_join") == 0) {
                        handle_permit_join_command(root);
                    } else {
                        ESP_LOGW(TAG, "Unknown command type: %s", type->valuestring);
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Invalid JSON command: %s", (const char *)buf);
            }
            pos = 0;
            memset(buf, 0, sizeof(buf));
            continue;
        }

        if (pos < (int)sizeof(buf) - 1) {
            buf[pos++] = ch;
        } else {
            ESP_LOGW(TAG, "Command line too long, dropping");
            pos = 0;
            memset(buf, 0, sizeof(buf));
        }
    }
}

static void config_report_temperature(void)
{
    ezb_zcl_config_report_record_t record[] = {
        {
            .direction = EZB_ZCL_REPORTING_SEND,
            .attr_id = 0x0000,
            .client = {
                .attr_type = EZB_ZCL_ATTR_TYPE_INT16,
                .min_interval = 1,
                .max_interval = 10,
                .reportable_change = { .s16 = 200 },
            },
        },
    };
    ezb_zcl_config_report_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
            .src_ep = COORD_ZB_HA_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
        },
        .payload.record_number = sizeof(record) / sizeof(record[0]),
        .payload.record_field = record,
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_zcl_config_report_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

static void config_report_battery(void)
{
    ezb_zcl_config_report_record_t record[] = {
        {
            .direction = EZB_ZCL_REPORTING_SEND,
            .attr_id = 0x0021,
            .client = {
                .attr_type = EZB_ZCL_ATTR_TYPE_UINT8,
                .min_interval = 30,
                .max_interval = 600,
                .reportable_change = { .u8 = 1 },
            },
        },
    };
    ezb_zcl_config_report_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_NONE,
            .src_ep = COORD_ZB_HA_EP_ID,
            .cluster_id = EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        },
        .payload.record_number = sizeof(record) / sizeof(record[0]),
        .payload.record_field = record,
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_zcl_config_report_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

static ezb_err_t write_thermostat_attr_short(uint16_t short_addr, uint8_t ep, uint16_t attr_id, uint8_t attr_type, const void *value)
{
    uint16_t attr_size = ezb_zcl_get_attr_value_size((ezb_zcl_attr_type_t)attr_type, value);
    if (attr_size == 0) {
        return EZB_ERR_INV_ARG;
    }

    ezb_zcl_attribute_t attrs[] = {
        {
            .id = attr_id,
            .data = {
                .type = attr_type,
                .size = attr_size,
                .value = (void *)value,
            },
        },
    };
    ezb_zcl_write_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
            .src_ep = COORD_ZB_HA_EP_ID,
            .dst_addr.u.short_addr = short_addr,
            .dst_ep = ep,
            .cluster_id = EZB_ZCL_CLUSTER_ID_THERMOSTAT,
        },
        .payload.attr_number = sizeof(attrs) / sizeof(attrs[0]),
        .payload.attr_field = attrs,
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t ret = ezb_zcl_write_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return ret;
}

static ezb_err_t write_thermostat_setpoint(uint16_t short_addr, uint8_t ep, float setpoint_c)
{
    int16_t setpoint_x100 = (int16_t)(setpoint_c * 100.0f);
    return write_thermostat_attr_short(short_addr, ep, 0x0012, EZB_ZCL_ATTR_TYPE_INT16, &setpoint_x100);
}

static ezb_err_t write_thermostat_system_mode(uint16_t short_addr, uint8_t ep, uint8_t system_mode)
{
    return write_thermostat_attr_short(short_addr, ep, 0x001C, EZB_ZCL_ATTR_TYPE_UINT8, &system_mode);
}

static ezb_err_t write_thermostat_preset_for_device(const char *model, uint16_t short_addr, uint8_t ep, const char *preset)
{
    uint8_t mode = 0x04;

    if (preset == NULL) {
        return EZB_ERR_FAIL;
    }

    if (model != NULL && strcmp(model, "trv701z") == 0) {
        if (strcmp(preset, "off") == 0 || strcmp(preset, "away") == 0 || strcmp(preset, "holiday") == 0) {
            mode = 0x00;
        } else if (strcmp(preset, "eco") == 0 || strcmp(preset, "auto") == 0 || strcmp(preset, "schedule") == 0 ||
                   strcmp(preset, "program") == 0) {
            mode = 0x01;
        } else if (strcmp(preset, "manual") == 0 || strcmp(preset, "comfort") == 0 || strcmp(preset, "boost") == 0 ||
                   strcmp(preset, "heat") == 0) {
            mode = 0x04;
        } else if (strcmp(preset, "sleep") == 0) {
            mode = 0x09;
        } else if (strcmp(preset, "cool") == 0) {
            mode = 0x03;
        } else if (strcmp(preset, "fan_only") == 0) {
            mode = 0x07;
        } else if (strcmp(preset, "dry") == 0) {
            mode = 0x08;
        } else {
            return EZB_ERR_FAIL;
        }
    } else if (strcmp(preset, "off") == 0 || strcmp(preset, "away") == 0 || strcmp(preset, "eco") == 0) {
        mode = 0x00;
    } else if (strcmp(preset, "auto") == 0 || strcmp(preset, "schedule") == 0 || strcmp(preset, "comfort") == 0 ||
               strcmp(preset, "boost") == 0 || strcmp(preset, "heat") == 0) {
        mode = 0x04;
    } else if (strcmp(preset, "cool") == 0) {
        mode = 0x03;
    } else if (strcmp(preset, "sleep") == 0) {
        mode = 0x09;
    } else if (strcmp(preset, "fan_only") == 0) {
        mode = 0x07;
    } else if (strcmp(preset, "dry") == 0) {
        mode = 0x08;
    } else if (strcmp(preset, "emergency_heating") == 0) {
        mode = 0x05;
    } else if (strcmp(preset, "precooling") == 0) {
        mode = 0x06;
    } else {
        return EZB_ERR_FAIL;
    }

    return write_thermostat_system_mode(short_addr, ep, mode);
}

static ezb_err_t subscribe_remote_cluster(uint16_t dst_short_addr, uint8_t dst_ep, uint16_t cluster_id)
{
    ezb_err_t ret = EZB_ERR_FAIL;
    ezb_zdo_bind_req_t *bind_req = malloc(sizeof(ezb_zdo_bind_req_t));
    if (bind_req == NULL) {
        return EZB_ERR_NO_MEM;
    }

    bind_req->dst_nwk_addr = dst_short_addr;
    bind_req->field.src_ep = dst_ep;
    bind_req->field.cluster_id = cluster_id;
    bind_req->field.dst_addr_mode = EZB_ADDR_MODE_EXT;
    bind_req->field.dst_ep = COORD_ZB_HA_EP_ID;
    bind_req->cb = zdo_bind_discovery_result;
    bind_req->user_ctx = bind_req;

    ezb_nwk_get_extended_address(&bind_req->field.dst_addr.extended_addr);
    ESP_RETURN_ON_ERROR(ezb_address_extended_by_short(dst_short_addr, &bind_req->field.src_addr), TAG,
                        "Failed to resolve local ext addr");

    ret = ezb_zdo_bind_req(bind_req);
    if (ret != EZB_ERR_NONE) {
        free(bind_req);
    }
    return ret;
}

static void configure_cluster_binds(uint16_t short_addr, uint8_t ep)
{
    const uint16_t clusters[] = {
        EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
        EZB_ZCL_CLUSTER_ID_THERMOSTAT,
        EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        EZB_ZCL_CLUSTER_ID_IAS_ZONE,
    };

    for (size_t i = 0; i < sizeof(clusters) / sizeof(clusters[0]); i++) {
        (void)bind_remote_cluster_async(short_addr, ep, clusters[i]);
        (void)subscribe_remote_cluster(short_addr, ep, clusters[i]);
    }

    config_report_temperature();
    config_report_battery();
}

static void zdo_bind_discovery_result(const ezb_zdp_bind_req_result_t *result, void *user_ctx)
{
    ezb_zdo_bind_req_t *bind_req = (ezb_zdo_bind_req_t *)user_ctx;
    assert(result);
    if (result->error == EZB_ERR_NONE) {
        if (result->rsp && result->rsp->status == EZB_ZDP_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Bind succeeded for cluster 0x%04hx", bind_req->field.cluster_id);
            if (bind_req->dst_nwk_addr == ezb_nwk_get_short_address()) {
                if (bind_req->field.cluster_id == EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT) {
                    config_report_temperature();
                }
                if (bind_req->field.cluster_id == EZB_ZCL_CLUSTER_ID_POWER_CONFIG) {
                    config_report_battery();
                }
            }
        } else {
            uint8_t status = result->rsp ? result->rsp->status : 0xff;
            ESP_LOGW(TAG, "Bind failed for cluster 0x%04hx status 0x%02x", bind_req->field.cluster_id, status);
        }
    } else {
        ESP_LOGW(TAG, "Bind error for cluster 0x%04hx error 0x%04x", bind_req->field.cluster_id, result->error);
    }

    if (user_ctx) {
        free(user_ctx);
    }
}

static ezb_err_t bind_remote_cluster_async(uint16_t dst_short_addr, uint8_t dst_ep, uint16_t cluster_id)
{
    ezb_err_t ret = EZB_ERR_FAIL;
    ezb_zdo_bind_req_t *bind_req = malloc(sizeof(ezb_zdo_bind_req_t));
    if (bind_req == NULL) {
        return EZB_ERR_NO_MEM;
    }

    bind_req->dst_nwk_addr = ezb_nwk_get_short_address();
    bind_req->field.src_ep = COORD_ZB_HA_EP_ID;
    bind_req->field.cluster_id = cluster_id;
    bind_req->field.dst_addr_mode = EZB_ADDR_MODE_EXT;
    bind_req->field.dst_ep = dst_ep;
    bind_req->cb = zdo_bind_discovery_result;
    bind_req->user_ctx = bind_req;

    ezb_nwk_get_extended_address(&bind_req->field.src_addr);
    ESP_RETURN_ON_ERROR(ezb_address_extended_by_short(dst_short_addr, &bind_req->field.dst_addr.extended_addr), TAG,
                        "Failed to resolve remote ext addr");

    ret = ezb_zdo_bind_req(bind_req);
    if (ret != EZB_ERR_NONE) {
        free(bind_req);
    }
    return ret;
}

static void read_manufacturer_and_model(uint16_t short_addr, uint8_t ep)
{
    uint16_t attr_field[] = {0x0004, 0x0005};
    ezb_zcl_read_attr_cmd_t cmd = {
        .cmd_ctrl = {
            .dst_addr.addr_mode = EZB_ADDR_MODE_SHORT,
            .src_ep = COORD_ZB_HA_EP_ID,
            .dst_addr.u.short_addr = short_addr,
            .dst_ep = ep,
            .cluster_id = EZB_ZCL_CLUSTER_ID_BASIC,
        },
        .payload.attr_number = sizeof(attr_field) / sizeof(attr_field[0]),
        .payload.attr_field = attr_field,
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_zcl_read_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
}

static void zdo_find_device_result(const ezb_zdo_match_desc_req_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (result->error != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Device discovery error 0x%04x", result->error);
        return;
    }
    if (!result->rsp || result->rsp->status != EZB_ZDP_STATUS_SUCCESS || result->rsp->match_length == 0 || !result->rsp->match_list) {
        return;
    }
    for (size_t i = 0; i < result->rsp->match_length; i++) {
        read_manufacturer_and_model(result->rsp->nwk_addr_of_interest, result->rsp->match_list[i]);
        configure_cluster_binds(result->rsp->nwk_addr_of_interest, result->rsp->match_list[i]);
    }
}

static void discover_device(uint16_t short_addr)
{
    uint16_t cluster_list[] = {
        EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
        EZB_ZCL_CLUSTER_ID_THERMOSTAT,
        EZB_ZCL_CLUSTER_ID_POWER_CONFIG,
        EZB_ZCL_CLUSTER_ID_IAS_ZONE,
    };

    ezb_zdo_match_desc_req_t req = {
        .dst_nwk_addr = short_addr,
        .field = {
            .nwk_addr_of_interest = short_addr,
            .profile_id = EZB_AF_HA_PROFILE_ID,
            .num_in_clusters = sizeof(cluster_list) / sizeof(cluster_list[0]),
            .num_out_clusters = 0,
            .cluster_list = cluster_list,
        },
        .cb = zdo_find_device_result,
        .user_ctx = NULL,
    };

    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_zdo_match_desc_req(&req);
    esp_zigbee_lock_release();
}

static void zcl_core_cmd_read_attr_rsp_handler(ezb_zcl_cmd_read_attr_rsp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    if (!message->in.header) {
        return;
    }

    const uint16_t short_addr = message->in.header->src_addr.u.short_addr;
    const uint8_t src_ep = message->in.header->src_ep;

    ESP_LOGI(TAG, "ZCL Read Attribute Response endpoint(%d) cluster(0x%04x)", message->info.dst_ep, message->info.cluster_id);

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_BASIC) {
        const ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables;
        const char *manufacturer = NULL;
        const char *model = NULL;
        while (var) {
            if (var->status == EZB_ZCL_STATUS_SUCCESS) {
                switch (var->attr_id) {
                case 0x0004:
                    manufacturer = (const char *)(var->attr_value + 1);
                    ESP_LOGI(TAG, "Manufacturer: %.*s", *(uint8_t *)var->attr_value, (char *)(var->attr_value + 1));
                    break;
                case 0x0005:
                    model = (const char *)(var->attr_value + 1);
                    ESP_LOGI(TAG, "Model: %.*s", *(uint8_t *)var->attr_value, (char *)(var->attr_value + 1));
                    break;
                default:
                    break;
                }
            }
            var = var->next;
        }
        publish_discovery_event(short_addr, src_ep, manufacturer, model);
        return;
    }

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT) {
        const ezb_zcl_read_attr_rsp_variable_t *var = message->in.variables;
        while (var) {
            if (var->status == EZB_ZCL_STATUS_SUCCESS && var->attr_id == 0x0000) {
                publish_trv_field(short_addr, src_ep, "local_temperature", (*(int16_t *)var->attr_value) / 100.0, NULL);
            }
            var = var->next;
        }
        return;
    }
}

static void zcl_core_cmd_report_attr_handler(ezb_zcl_cmd_report_attr_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    if (!message->in.header) {
        return;
    }

    const uint16_t short_addr = message->in.header->src_addr.u.short_addr;
    const uint8_t src_ep = message->in.header->src_ep;
    const ezb_zcl_report_attr_variable_t *var = message->in.variables;

    switch (message->info.cluster_id) {
    case EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT:
        while (var) {
            if (var->attr_id == 0x0000) {
                publish_trv_field(short_addr, src_ep, "local_temperature", (*(int16_t *)var->attr_value) / 100.0, NULL);
            }
            var = var->next;
        }
        break;
    case EZB_ZCL_CLUSTER_ID_THERMOSTAT:
        while (var) {
            if (var->attr_id == 0x0000) {
                publish_trv_field(short_addr, src_ep, "local_temperature", (*(int16_t *)var->attr_value) / 100.0, NULL);
            } else if (var->attr_id == 0x0012) {
                publish_trv_field(short_addr, src_ep, "occupied_heating_setpoint", (*(int16_t *)var->attr_value) / 100.0, NULL);
            } else if (var->attr_id == 0x0014) {
                publish_trv_field(short_addr, src_ep, "unoccupied_heating_setpoint", (*(int16_t *)var->attr_value) / 100.0, NULL);
            } else if (var->attr_id == 0x001C) {
                uint8_t mode = *(uint8_t *)var->attr_value;
                publish_trv_field(short_addr, src_ep, "system_mode", (double)mode, thermostat_system_mode_name(mode));
            } else if (var->attr_id == 0x0029) {
                uint8_t state = *(uint8_t *)var->attr_value;
                publish_trv_field(short_addr, src_ep, "running_state", (double)state, state ? "heat" : "idle");
            } else if (var->attr_id == 0x0008) {
                publish_trv_field(short_addr, src_ep, "pi_heating_demand", (*(uint8_t *)var->attr_value), NULL);
            }
            var = var->next;
        }
        break;
    case EZB_ZCL_CLUSTER_ID_REL_HUMIDITY_MEASUREMENT:
        while (var) {
            if (var->attr_id == 0x0000) {
                publish_trv_field(short_addr, src_ep, "humidity", (*(uint16_t *)var->attr_value) / 100.0, NULL);
            }
            var = var->next;
        }
        break;
    case EZB_ZCL_CLUSTER_ID_POWER_CONFIG:
        while (var) {
            if (var->attr_id == 0x0021) {
                publish_trv_field(short_addr, src_ep, "battery", (*(uint8_t *)var->attr_value) / 2.0, NULL);
            } else if (var->attr_id == 0x0001) {
                publish_trv_field(short_addr, src_ep, "battery_voltage", (*(uint8_t *)var->attr_value) / 10.0, NULL);
            }
            var = var->next;
        }
        break;
    default:
        ESP_LOGW(TAG, "Unhandled report cluster 0x%04x", message->info.cluster_id);
        break;
    }
}

static void zcl_core_cmd_report_config_rsp_handler(ezb_zcl_cmd_config_report_rsp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    ESP_LOGI(TAG, "ZCL Report Config Response cluster(0x%04x) status(0x%02x)", message->info.cluster_id, message->info.status);
}

static void zcl_core_cmd_default_rsp_handler(ezb_zcl_cmd_default_rsp_message_t *message)
{
    ESP_RETURN_ON_FALSE(message, , TAG, "message is empty");
    ESP_LOGI(TAG, "ZCL Default Response status(0x%02x)", message->in.status_code);
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID:
        zcl_core_cmd_read_attr_rsp_handler(message);
        break;
    case EZB_ZCL_CORE_CONFIG_REPORT_RSP_CB_ID:
        zcl_core_cmd_report_config_rsp_handler(message);
        break;
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID:
        zcl_core_cmd_report_attr_handler(message);
        break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID:
        zcl_core_cmd_default_rsp_handler(message);
        break;
    default:
        ESP_LOGW(TAG, "Unhandled ZCL core action 0x%04lx", callback_id);
        break;
    }
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);
    union {
        const ezb_bdb_signal_simple_params_t *bdb;
        const ezb_zdo_signal_leave_indication_params_t *leave_ind;
        const ezb_zdo_signal_device_annce_params_t *dev_annce;
        const ezb_nwk_signal_permit_join_status_params_t *permit_join;
        const void *param;
    } signal_params = { .param = ezb_app_signal_get_params(app_signal) };

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (signal_params.bdb->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
            } else {
                ezb_bdb_open_network(180);
            }
        } else {
            ESP_LOGW(TAG, "Startup failed with status 0x%02x", signal_params.bdb->status);
        }
        break;
    case EZB_BDB_SIGNAL_FORMATION:
        if (signal_params.bdb->status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Formed network (PAN 0x%04hx, EXT 0x%llx, channel %d, short 0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGW(TAG, "Formation failed with status 0x%02x", signal_params.bdb->status);
        }
        break;
    case EZB_BDB_SIGNAL_STEERING:
        if (signal_params.bdb->status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Network steering completed");
        } else {
            ESP_LOGW(TAG, "Steering failed with status 0x%02x", signal_params.bdb->status);
        }
        break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE:
        ESP_LOGI(TAG, "Device announce short 0x%04hx", signal_params.dev_annce->short_addr);
        discover_device(signal_params.dev_annce->short_addr);
        break;
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION:
        ESP_LOGI(TAG, "Device leaving short 0x%04hx", signal_params.leave_ind->short_addr);
        break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
        if (signal_params.permit_join->duration) {
            ESP_LOGI(TAG, "Network open for %d seconds", signal_params.permit_join->duration);
        } else {
            ESP_LOGW(TAG, "Network closed to joins");
        }
        break;
    default:
        ESP_LOGI(TAG, "Zigbee signal: %s (0x%x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static esp_err_t create_gateway_collector_device(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_thermostat_config_t thermostat_cfg = EZB_ZHA_THERMOSTAT_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_thermostat(COORD_ZB_HA_EP_ID, &thermostat_cfg);
    ezb_zcl_cluster_desc_t basic_desc = { 0 };

    basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)COORD_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)COORD_MODEL_IDENTIFIER);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc,
                                                     ezb_zcl_temperature_measurement_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)));

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));
    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

static esp_err_t setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(COORD_ZB_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(COORD_ZB_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));
    return ESP_OK;
}

static void zigbee_stack_task(void *pvParameters)
{
    (void)pvParameters;
    esp_zigbee_config_t config = COORD_ZB_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(setup_commissioning());
    ESP_ERROR_CHECK(create_gateway_collector_device());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_flash_init_partition(COORD_ZB_STORAGE_PARTITION_NAME));

    uart_init();
    ESP_LOGI(TAG, "Start real Zigbee stack");
    xTaskCreate(zigbee_stack_task, "Zigbee_main", 8192, NULL, 5, NULL);
    xTaskCreate(command_task, "command_task", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat_task", 2048, NULL, 4, NULL);
    publish_mode_status();
}
static void sanitize_name_component(const char *src, char *dst, size_t dst_size)
{
    size_t i;
    size_t out = 0;
    bool prev_underscore = false;

    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    for (i = 0; src[i] != '\0' && out + 1 < dst_size; i++) {
        char ch = src[i];
        if ((ch >= 'A' && ch <= 'Z')) {
            ch = (char)(ch - 'A' + 'a');
        }
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
            ch = '_';
        }
        if (ch == '_') {
            if (prev_underscore || out == 0) {
                continue;
            }
            prev_underscore = true;
        } else {
            prev_underscore = false;
        }
        dst[out++] = ch;
    }

    while (out > 0 && dst[out - 1] == '_') {
        out--;
    }
    dst[out] = '\0';
}

static const char *thermostat_system_mode_name(uint8_t mode)
{
    switch (mode) {
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF:
        return "off";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_AUTO:
        return "auto";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL:
        return "cool";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT:
        return "heat";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_EMERGENCY_HEATING:
        return "emergency_heating";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_PRECOOLING:
        return "precooling";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY:
        return "fan_only";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY:
        return "dry";
    case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_SLEEP:
        return "sleep";
    default:
        return "unknown";
    }
}
