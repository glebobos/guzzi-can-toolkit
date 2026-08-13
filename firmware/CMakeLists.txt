cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(guzzi_can_toolkit)

# Create SPIFFS image partition from spiffs_image directory
spiffs_create_partition_image(storage spiffs_image FLASH_IN_PROJECT)
