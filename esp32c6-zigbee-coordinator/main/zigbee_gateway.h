#pragma once

#include "esp_zigbee.h"

#define COORD_ZB_STORAGE_PARTITION_NAME "zb_storage"
#define COORD_ZB_HA_EP_ID               1

#define COORD_ZB_PRIMARY_CHANNEL_MASK    (1U << 13)
#define COORD_ZB_SECONDARY_CHANNEL_MASK  0

#define COORD_MANUFACTURER_NAME "\x04""GBAH"
#define COORD_MODEL_IDENTIFIER  "\x07""DUAL_C6"

#define COORD_ZB_ZC_CONFIG()                          \
    {                                                 \
        .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, \
        .install_code_policy = false,                 \
        .zczr_config = {                              \
            .max_children = 10,                       \
        },                                            \
    }

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define COORD_ZB_PLATFORM_CONFIG()                    \
    {                                                 \
        .storage_partition_name = COORD_ZB_STORAGE_PARTITION_NAME, \
        .radio_config = {                             \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE, \
        },                                            \
    }
#else
#error "ESP32-C6 Zigbee requires IEEE 802.15.4 support"
#endif

#define COORD_ZB_DEFAULT_CONFIG()                     \
    {                                                 \
        .device_config = COORD_ZB_ZC_CONFIG(),        \
        .platform_config = COORD_ZB_PLATFORM_CONFIG(),\
    }
