cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

add_compile_definitions(
    EPUB_LOG_LEVEL=ESP_LOG_DEBUG
    DATE_TIME_RTC=1
    PNGLE_GRAYSCALE_OUTPUT=1
    PNGLE_NO_GAMMA_CORRECTION=1
)

if(DEVICE MATCHES "^INKPLATE_6$")
    message("INKPLATE 6 defined")
    set(DEVICE_NAME "I6")
    add_compile_definitions(
        INKPLATE_6=1
        MCP23017=1)
elseif(DEVICE MATCHES "^INKPLATE_5V2$")
    message("INKPLATE 5V2 defined")
    set(DEVICE_NAME "I5V2")
    add_compile_definitions(
        INKPLATE_5V2=1
        PCAL6416=1)
elseif(DEVICE MATCHES  "^INKPLATE_6PLUS$")
    message("INKPLATE 6PLUS defined")
    set(DEVICE_NAME "I6P")
    add_compile_definitions(
        INKPLATE_6PLUS=1 
        MCP23017=1)
elseif(DEVICE MATCHES "^INKPLATE_6PLUS_V2$")
    message("INKPLATE 6PLUS V2 defined")
    set(DEVICE_NAME "I6PV2")
    add_compile_definitions(
        INKPLATE_6PLUS_V2=1 
        PCAL6416=1)
elseif(DEVICE MATCHES "^INKPLATE_6FLICK$")
    message("INKPLATE 6FLICK defined")
    set(DEVICE_NAME "I6F")
    add_compile_definitions(
        INKPLATE_6FLICK=1 
        PCAL6416=1)
elseif(DEVICE MATCHES "^INKPLATE_10$")
    message("INKPLATE 10 defined")
    set(DEVICE_NAME "I10")
    add_compile_definitions(
        INKPLATE_10=1 
        MCP23017=1)
elseif(DEVICE MATCHES "^INKPLATE_6_V2$")
    message("INKPLATE 6 V2 defined")
    set(DEVICE_NAME "I6V2")
    add_compile_definitions(
        INKPLATE_6_V2=1
        PCAL6416=1)
elseif(DEVICE MATCHES "^INKPLATE_5V2$")
    message("INKPLATE 5 V2 defined")
    set(DEVICE_NAME "I5V2")
    add_compile_definitions(
        INKPLATE_5V2=1
        PCAL6416=1)
else()
    message(FATAL_ERROR "DEVICE NOT defined!!\nExample call: idf.py build -DDEVICE=INKPLATE_6 -DAPP_VERSION=2.1.0")
endif()

list(APPEND EXTRA_COMPONENT_DIRS 
    src 
)

string(TIMESTAMP THE_TIME "%Y%m%d-%H%MZ" UTC)

set(PROJECT_VER "${APP_VERSION}-${DEVICE_NAME}-${THE_TIME}") 

add_compile_definitions(
    APP_VERSION="${APP_VERSION}"
    EPUB_INKPLATE_BUILD=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EPub-InkPlate)

# LittleFS image for single_book builds (Phase 2).
# Populated from littlefs_root/ in the repository — copy book, fonts, and
# fonts_list.xml there before building.
#
# NOT using FLASH_IN_PROJECT so that `idf.py flash` only flashes the app.
# Flash the storage partition separately (book/fonts update without reflashing app):
#   esptool.py --chip esp32 -p /dev/ttyUSB0 write_flash 0x310000 build_single/storage.bin
#
if(BUILD_VARIANT STREQUAL "single_book")
    littlefs_create_partition_image(storage ${CMAKE_CURRENT_SOURCE_DIR}/littlefs_root)
endif()
