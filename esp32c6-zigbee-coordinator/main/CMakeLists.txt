idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver freertos json esp_timer nvs_flash espressif__esp-zigbee-lib
)
