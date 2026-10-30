cmake_minimum_required(VERSION 3.16)

# Every build directory carries its own sdkconfig (see make_espVT.sh), so the
# board and feature variants never overwrite each other's configuration.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(DEPENDENCIES_LOCK dependencies.lock.${IDF_TARGET})
if(NOT IDF_TARGET STREQUAL "esp32p4")
    # Touch driver of the ESP32-P4 board; its registry dependencies are P4-only.
    list(APPEND EXCLUDE_COMPONENTS esp_lcd_touch_gsl3680)
endif()

project(esp_isobus_vt)
