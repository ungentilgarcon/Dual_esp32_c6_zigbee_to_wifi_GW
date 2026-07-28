#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_spiffs.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "bridge_config.h"

static const char *TAG = "WIFI_BRIDGE";
static esp_mqtt_client_handle_t s_mqtt = NULL;
typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char mqtt_uri[128];
    char mqtt_base_topic[64];
} bridge_runtime_config_t;

static bridge_runtime_config_t s_cfg = {
    .wifi_ssid = BRIDGE_WIFI_SSID,
    .wifi_pass = BRIDGE_WIFI_PASS,
    .mqtt_uri = BRIDGE_MQTT_URI,
    .mqtt_base_topic = BRIDGE_MQTT_BASE_TOPIC,
};

static bool copy_json_string_field(cJSON *root, const char *field, char *dst, size_t dst_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, field);
    int written;

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }

    written = snprintf(dst, dst_size, "%s", item->valuestring);
    if (written < 0 || written >= (int)dst_size) {
        ESP_LOGW(TAG, "Config field %s is too long", field);
        return false;
    }

    return true;
}

static void load_default_config(void) {
    snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "%s", BRIDGE_WIFI_SSID);
    snprintf(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), "%s", BRIDGE_WIFI_PASS);
    snprintf(s_cfg.mqtt_uri, sizeof(s_cfg.mqtt_uri), "%s", BRIDGE_MQTT_URI);
    snprintf(s_cfg.mqtt_base_topic, sizeof(s_cfg.mqtt_base_topic), "%s", BRIDGE_MQTT_BASE_TOPIC);
}

static void mount_spiffs(void) {
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    size_t total = 0;
    size_t used = 0;

    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
    ESP_ERROR_CHECK(esp_spiffs_info(conf.partition_label, &total, &used));
    ESP_LOGI(TAG, "SPIFFS mounted: total=%u used=%u", (unsigned)total, (unsigned)used);
}

static void load_bridge_config(void) {
    FILE *fp;
    long file_size;
    char *buffer;
    cJSON *root;

    load_default_config();
    mount_spiffs();

    fp = fopen(BRIDGE_CONFIG_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "Config file %s not found, using defaults", BRIDGE_CONFIG_PATH);
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "Failed to seek config file");
        fclose(fp);
        return;
    }

    file_size = ftell(fp);
    if (file_size <= 0 || file_size > 4096) {
        ESP_LOGW(TAG, "Config file size is invalid");
        fclose(fp);
        return;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Failed to rewind config file");
        fclose(fp);
        return;
    }

    buffer = malloc((size_t)file_size + 1U);
    if (buffer == NULL) {
        ESP_LOGW(TAG, "Out of memory loading config file");
        fclose(fp);
        return;
    }

    if (fread(buffer, 1, (size_t)file_size, fp) != (size_t)file_size) {
        ESP_LOGW(TAG, "Failed to read config file");
        free(buffer);
        fclose(fp);
        return;
    }
    buffer[file_size] = '\0';
    fclose(fp);

    root = cJSON_Parse(buffer);
    free(buffer);
    if (root == NULL || !cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Invalid JSON in %s", BRIDGE_CONFIG_PATH);
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return;
    }

    copy_json_string_field(root, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    copy_json_string_field(root, "wifi_password", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    copy_json_string_field(root, "mqtt_uri", s_cfg.mqtt_uri, sizeof(s_cfg.mqtt_uri));
    copy_json_string_field(root, "mqtt_base_topic", s_cfg.mqtt_base_topic, sizeof(s_cfg.mqtt_base_topic));
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded Wi-Fi config from %s", BRIDGE_CONFIG_PATH);
}

static void uart_init(void) {
    const uart_config_t cfg = {
        .baud_rate = BRIDGE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_BUF_SIZE, BRIDGE_UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BRIDGE_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, BRIDGE_UART_TX_PIN, BRIDGE_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, s_cfg.wifi_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_cfg.wifi_pass, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void uart_write_line(const char *line) {
    uart_write_bytes(BRIDGE_UART_PORT, line, strlen(line));
    uart_write_bytes(BRIDGE_UART_PORT, "\n", 1);
}

static void forward_set_command_to_uart(const char *topic, const char *payload, int payload_len) {
    const size_t prefix_len = strlen(s_cfg.mqtt_base_topic) + 1;
    const size_t topic_len = strlen(topic);
    const char suffix[] = "/set";

    if (topic_len <= prefix_len + strlen(suffix)) {
        return;
    }
    if (strncmp(topic, s_cfg.mqtt_base_topic, strlen(s_cfg.mqtt_base_topic)) != 0 || topic[strlen(s_cfg.mqtt_base_topic)] != '/') {
        return;
    }
    if (strcmp(topic + topic_len - strlen(suffix), suffix) != 0) {
        return;
    }

    size_t name_len = topic_len - prefix_len - strlen(suffix);
    if (name_len == 0 || name_len >= 96) {
        return;
    }

    char friendly_name[96];
    memset(friendly_name, 0, sizeof(friendly_name));
    memcpy(friendly_name, topic + prefix_len, name_len);

    char payload_buf[512];
    if (payload_len < 0 || payload_len >= (int)sizeof(payload_buf)) {
        ESP_LOGW(TAG, "MQTT payload too large");
        return;
    }
    memcpy(payload_buf, payload, payload_len);
    payload_buf[payload_len] = '\0';

    cJSON *payload_json = cJSON_Parse(payload_buf);
    if (payload_json == NULL || !cJSON_IsObject(payload_json)) {
        ESP_LOGW(TAG, "Ignoring invalid JSON payload on %s", topic);
        if (payload_json != NULL) {
            cJSON_Delete(payload_json);
        }
        return;
    }

    cJSON *inner_type = cJSON_GetObjectItemCaseSensitive(payload_json, "type");
    if (cJSON_IsString(inner_type) && inner_type->valuestring != NULL && strcmp(inner_type->valuestring, "test") == 0) {
        cJSON_Delete(payload_json);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "set");
    cJSON_AddStringToObject(root, "friendly_name", friendly_name);
    cJSON_AddItemToObject(root, "payload", payload_json);

    char *encoded = cJSON_PrintUnformatted(root);
    if (encoded != NULL) {
        uart_write_line(encoded);
        ESP_LOGI(TAG, "Forwarded MQTT set for %s", friendly_name);
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
}

static void forward_test_command_to_uart(const char *topic, const char *payload, int payload_len) {
    const size_t prefix_len = strlen(s_cfg.mqtt_base_topic) + 1;
    const char suffix[] = "/set";
    const size_t topic_len = strlen(topic);

    if (strncmp(topic, s_cfg.mqtt_base_topic, strlen(s_cfg.mqtt_base_topic)) != 0 || topic[strlen(s_cfg.mqtt_base_topic)] != '/') {
        return;
    }
    if (topic_len <= prefix_len + strlen(suffix)) {
        return;
    }
    if (strcmp(topic + topic_len - strlen(suffix), suffix) != 0) {
        return;
    }

    size_t name_len = topic_len - prefix_len - strlen(suffix);
    if (name_len == 0 || name_len >= 96) {
        return;
    }

    char friendly_name[96];
    memset(friendly_name, 0, sizeof(friendly_name));
    memcpy(friendly_name, topic + prefix_len, name_len);

    char payload_buf[512];
    if (payload_len < 0 || payload_len >= (int)sizeof(payload_buf)) {
        ESP_LOGW(TAG, "MQTT payload too large");
        return;
    }
    memcpy(payload_buf, payload, payload_len);
    payload_buf[payload_len] = '\0';

    cJSON *payload_json = cJSON_Parse(payload_buf);
    if (payload_json == NULL || !cJSON_IsObject(payload_json)) {
        if (payload_json != NULL) {
            cJSON_Delete(payload_json);
        }
        return;
    }

    cJSON *inner_type = cJSON_GetObjectItemCaseSensitive(payload_json, "type");
    if (!cJSON_IsString(inner_type) || inner_type->valuestring == NULL || strcmp(inner_type->valuestring, "test") != 0) {
        cJSON_Delete(payload_json);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "set");
    cJSON_AddStringToObject(root, "friendly_name", friendly_name);
    cJSON_AddItemToObject(root, "payload", payload_json);

    char *encoded = cJSON_PrintUnformatted(root);
    if (encoded != NULL) {
        uart_write_line(encoded);
        ESP_LOGI(TAG, "Forwarded test command for %s", friendly_name);
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
}

static void forward_mode_command_to_uart(const char *payload, int payload_len) {
    char payload_buf[256];
    if (payload_len < 0 || payload_len >= (int)sizeof(payload_buf)) {
        ESP_LOGW(TAG, "Mode payload too large");
        return;
    }
    memcpy(payload_buf, payload, payload_len);
    payload_buf[payload_len] = '\0';

    cJSON *payload_json = cJSON_Parse(payload_buf);
    if (payload_json == NULL || !cJSON_IsObject(payload_json)) {
        if (payload_json != NULL) {
            cJSON_Delete(payload_json);
        }
        ESP_LOGW(TAG, "Invalid mode payload");
        return;
    }

    cJSON *mode = cJSON_GetObjectItemCaseSensitive(payload_json, "mode");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
        cJSON_Delete(payload_json);
        ESP_LOGW(TAG, "Missing mode field");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mode");
    cJSON_AddStringToObject(root, "mode", mode->valuestring);

    char *encoded = cJSON_PrintUnformatted(root);
    if (encoded != NULL) {
        uart_write_line(encoded);
        ESP_LOGI(TAG, "Forwarded mode command: %s", mode->valuestring);
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    cJSON_Delete(payload_json);
}

static int topic_is_gateway_control_set(const char *topic) {
    char expected[128];
    snprintf(expected, sizeof(expected), "%s/gateway_control/set", s_cfg.mqtt_base_topic);
    return strcmp(topic, expected) == 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    if (event_id == MQTT_EVENT_CONNECTED) {
        char topic[128];
        snprintf(topic, sizeof(topic), "%s/+/set", s_cfg.mqtt_base_topic);
        esp_mqtt_client_subscribe(s_mqtt, topic, 1);
        ESP_LOGI(TAG, "MQTT connected and subscribed to %s", topic);
        return;
    }

    if (event_id == MQTT_EVENT_DATA) {
        char topic[128];
        int topic_len = event->topic_len;
        if (topic_len >= (int)sizeof(topic)) {
            topic_len = sizeof(topic) - 1;
        }
        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';

        if (topic_is_gateway_control_set(topic)) {
            forward_mode_command_to_uart(event->data, event->data_len);
            return;
        }

        forward_test_command_to_uart(topic, event->data, event->data_len);
        forward_set_command_to_uart(topic, event->data, event->data_len);
        return;
    }
}

static void mqtt_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_cfg.mqtt_uri,
    };

    s_mqtt = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt));
}

static void publish_uart_telemetry_to_mqtt(const char *line) {
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "Invalid UART JSON frame");
        return;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *friendly_name = cJSON_GetObjectItemCaseSensitive(root, "friendly_name");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "trv_telemetry") == 0 && cJSON_IsString(friendly_name) && friendly_name->valuestring != NULL) {
        char topic[192];
        snprintf(topic, sizeof(topic), "%s/%s", s_cfg.mqtt_base_topic, friendly_name->valuestring);

        cJSON_DeleteItemFromObjectCaseSensitive(root, "type");
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published telemetry to %s", topic);
            cJSON_free(payload);
        }
    } else if (strcmp(type->valuestring, "test_result") == 0 && cJSON_IsString(friendly_name) && friendly_name->valuestring != NULL) {
        char topic[192];
        snprintf(topic, sizeof(topic), "%s/%s/result", s_cfg.mqtt_base_topic, friendly_name->valuestring);
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published test result to %s", topic);
            cJSON_free(payload);
        }
    } else if (strcmp(type->valuestring, "test_event") == 0 && cJSON_IsString(friendly_name) && friendly_name->valuestring != NULL) {
        char topic[192];
        snprintf(topic, sizeof(topic), "%s/%s/event", s_cfg.mqtt_base_topic, friendly_name->valuestring);
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published test event to %s", topic);
            cJSON_free(payload);
        }
    } else if (strcmp(type->valuestring, "mode_status") == 0 && cJSON_IsString(friendly_name) && friendly_name->valuestring != NULL) {
        char topic[192];
        snprintf(topic, sizeof(topic), "%s/%s/status", s_cfg.mqtt_base_topic, friendly_name->valuestring);
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published mode status to %s", topic);
            cJSON_free(payload);
        }
    } else if (strcmp(type->valuestring, "zigbee_event") == 0 && cJSON_IsString(friendly_name) && friendly_name->valuestring != NULL) {
        char topic[192];
        snprintf(topic, sizeof(topic), "%s/bridge/event", s_cfg.mqtt_base_topic);
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            ESP_LOGI(TAG, "Published zigbee event to %s", topic);
            cJSON_free(payload);
        }
    } else if (strcmp(type->valuestring, "network") == 0) {
        char *payload = cJSON_PrintUnformatted(root);
        if (payload != NULL) {
            char topic[192];
            snprintf(topic, sizeof(topic), "%s/bridge/event", s_cfg.mqtt_base_topic);
            esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, 0);
            cJSON_free(payload);
        }
    }

    cJSON_Delete(root);
}

static void uart_rx_task(void *arg) {
    (void)arg;
    uint8_t buf[BRIDGE_UART_BUF_SIZE];
    int pos = 0;
    memset(buf, 0, sizeof(buf));

    while (1) {
        uint8_t ch;
        int n = uart_read_bytes(BRIDGE_UART_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) {
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            if (pos == 0) {
                continue;
            }
            buf[pos] = '\0';
            publish_uart_telemetry_to_mqtt((const char *)buf);
            pos = 0;
            memset(buf, 0, sizeof(buf));
            continue;
        }

        if (pos < (int)sizeof(buf) - 1) {
            buf[pos++] = ch;
        } else {
            ESP_LOGW(TAG, "UART line too long, dropping");
            pos = 0;
            memset(buf, 0, sizeof(buf));
        }
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    uart_init();
    load_bridge_config();
    wifi_init_sta();
    mqtt_start();
    xTaskCreate(uart_rx_task, "uart_rx_task", 6144, NULL, 5, NULL);
}
