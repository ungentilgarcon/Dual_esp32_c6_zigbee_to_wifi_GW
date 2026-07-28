cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_wifi_bridge)
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
