if(CONFIG_ISOVT_ENABLE_AGISOSTACK)
    # AgIsoStack++ is a regular CMake project, so include it before this
    # component links against its targets. TWAI is the ESP32 CAN controller.
    set(CAN_DRIVER "TWAI")
    set(CAN_STACK_DISABLE_THREADS OFF)
    add_subdirectory(
        "${CMAKE_SOURCE_DIR}/third_party/AgIsoStack-plus-plus"
        "${CMAKE_BINARY_DIR}/agisostack"
    )
    target_compile_definitions(HardwareIntegration PRIVATE ESP_PLATFORM)
endif()

set(ISOVT_SOURCES
    "app_main.cpp"
    "aux_n_manager.cpp"
    "can_monitor.cpp"
    "gnss_um982.cpp"
    "isobus_terminal_server.cpp"
    "lcd_panel.cpp"
    "lvgl_terminal_view.cpp"
    "logo_image.cpp"
    "object_pool_store.cpp"
    "section_control.cpp"
    "stb_truetype_impl.c"
    "vt_audio.cpp"
    "ui_text.cpp"
    "vt_font.cpp"
    "vt_scene_renderer.cpp"
)
# Requirements are resolved before sdkconfig is loaded, so they follow the chip.
set(ISOVT_BOARD_REQUIRES)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND ISOVT_BOARD_REQUIRES esp_driver_ppa esp_lcd_touch_gsl3680)
endif()
if(CONFIG_ISOVT_BOARD_GUITION_JC8012P4A1C)
    list(APPEND ISOVT_SOURCES
        "board_support_jc8012p4.cpp"
        "jc8012_lcd_init.c"
        "lcd_backend_dsi.cpp"
        "touch_gsl3680.cpp"
    )
else()
    list(APPEND ISOVT_SOURCES
        "board_support_waveshare7b.cpp"
        "lcd_backend_rgb.cpp"
        "touch_gt911.cpp"
    )
endif()

idf_component_register(
    SRCS
        ${ISOVT_SOURCES}
    INCLUDE_DIRS
        "."
    PRIV_INCLUDE_DIRS
        "../third_party/stb"
    REQUIRES
        driver
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_uart
        esp_event
        esp_lcd
        esp_mm
        esp_netif
        esp_timer
        esp_wifi
        lwip
        mbedtls
        nvs_flash
        pthread
        spiffs
        ${ISOVT_BOARD_REQUIRES}
    EMBED_FILES
        "../assets/fonts/RobotoMono-Regular.ttf"
)

# Third-party rasterizer, compiled as shipped.
set_source_files_properties("stb_truetype_impl.c" PROPERTIES COMPILE_OPTIONS "-w")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LOGO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(LOGO_HEADER "${LOGO_GENERATED_DIR}/logo_rgb565.hpp")
# The boot logo is drawn in the UI area; on the 10.1" board that area is scaled
# by 5/4, so the asset is generated at that size and copied 1:1.
if(CONFIG_ISOVT_BOARD_GUITION_JC8012P4A1C)
    set(LOGO_MAX_WIDTH 900)
    set(LOGO_MAX_HEIGHT 350)
    set(LOGO_PADDING 15)
else()
    set(LOGO_MAX_WIDTH 720)
    set(LOGO_MAX_HEIGHT 280)
    set(LOGO_PADDING 12)
endif()
add_custom_command(
    OUTPUT "${LOGO_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LOGO_GENERATED_DIR}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/bmp_to_rgb565.py"
            "${CMAKE_SOURCE_DIR}/logo.png" "${LOGO_HEADER}" --max-width ${LOGO_MAX_WIDTH} --max-height ${LOGO_MAX_HEIGHT} --padding ${LOGO_PADDING}
    DEPENDS "${CMAKE_SOURCE_DIR}/logo.png" "${CMAKE_SOURCE_DIR}/tools/bmp_to_rgb565.py" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Preparing RGB565 boot logo"
    VERBATIM
)
add_custom_target(isovt_logo_asset DEPENDS "${LOGO_HEADER}")
add_dependencies(${COMPONENT_LIB} isovt_logo_asset)
target_include_directories(${COMPONENT_LIB} PRIVATE "${LOGO_GENERATED_DIR}")

if(CONFIG_ISOVT_ENABLE_AGISOSTACK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ISOVT_ENABLE_AGISOSTACK=1)
    target_compile_definitions(Isobus PUBLIC ISOVT_ESP_POOL_THREADS=1)
    target_link_libraries(Isobus PRIVATE idf::pthread idf::heap)
    target_link_libraries(${COMPONENT_LIB} PRIVATE isobus::Isobus isobus::HardwareIntegration isobus::Utility)
endif()
