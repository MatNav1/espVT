#include "board_support.hpp"
#include "can_monitor.hpp"
#include "gnss_um982.hpp"
#include "isobus_terminal_server.hpp"
#include "lvgl_terminal_view.hpp"
#include "object_pool_store.hpp"
#include "section_control.hpp"
#include "touch_gt911.hpp"
#include "vt_audio.hpp"
#include "vt_scene_renderer.hpp"

#include <cstdint>

#include "esp_log.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace
{
constexpr char TAG[] = "app";

void start_terminal_when_bus_is_active(void *)
{
    ESP_LOGI(TAG, "Waiting for first CAN frame before starting VT traffic");
    while (!can_monitor::bus_activity_detected())
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "CAN traffic detected; starting ISOBUS VT");
    const bool stack_ready = isovt::start();
    if (stack_ready)
    {
        can_monitor::request_address_claims();
    }
    vt_view::set_bus_state(true, stack_ready);
    if (!stack_ready)
    {
        vt_view::set_status("ISOBUS VT start failed");
    }
    vTaskDelete(nullptr);
}
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Starting ESP32-S3 ISOBUS VT");

    // AgIsoStack++ creates nested C++ threads for CAN and object-pool parsing.
    auto pthread_config = esp_pthread_get_default_config();
    pthread_config.stack_size = 24576;
    pthread_config.inherit_cfg = true;
    pthread_config.thread_name = "isobus";
    pthread_config.pin_to_core = 1;
    ESP_ERROR_CHECK(esp_pthread_set_cfg(&pthread_config));

    esp_err_t result = nvs_flash_init();
    if ((result == ESP_ERR_NVS_NO_FREE_PAGES) || (result == ESP_ERR_NVS_NEW_VERSION_FOUND))
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    ESP_ERROR_CHECK(board::init_i2c());
    ESP_ERROR_CHECK(board::init_io_expander());
    ESP_ERROR_CHECK(board::enable_lcd_power());

    // Claim GPIO19/20 for TWAI before the native USB peripheral can retain the
    // shared pins after a cold power-on.
    const bool can_ready = (can_monitor::start() == ESP_OK);

    // SPIFFS reads temporarily disable cache access; complete the mount before RGB starts reading PSRAM.
    const bool storage_ready = object_pool_store::init();
    ESP_LOGI(TAG, "Object pool storage: %s", storage_ready ? "ready" : "not ready");

    vt_view::init();
    can_monitor::enable_ui_updates();
    vt_view::set_status("Initializing ISOBUS terminal");

    const bool touch_ready = (touch_gt911::start() == ESP_OK);
    ESP_LOGI(TAG, "GT911 touch: %s", touch_ready ? "ready" : "not ready");

    const bool local_ui_ready = isovt::start_local_ui();
    ESP_LOGI(TAG, "Local VT menu: %s", local_ui_ready ? "ready" : "not ready");

    const esp_err_t audio_result = vt_audio::start();
    const auto terminal_settings = vt_scene_renderer::get_terminal_settings();
    vt_audio::set_control_volume(terminal_settings.control_volume);
    vt_audio::set_alarm_volume(terminal_settings.alarm_volume);
    vt_audio::set_muted(terminal_settings.audio_muted);
    ESP_LOGI(TAG, "VT audio: %s", vt_audio::is_enabled() ? "ready" : esp_err_to_name(audio_result));

    const esp_err_t gnss_result = gnss_um982::start();
    if (gnss_um982::is_enabled())
    {
        ESP_LOGI(TAG, "External GNSS: %s", esp_err_to_name(gnss_result));
    }
    ESP_LOGI(TAG, "Section-control safety gate: %s", section_control::start() ? "ready" : "not ready");

    if (can_ready)
    {
        if (xTaskCreatePinnedToCore(start_terminal_when_bus_is_active,
                                    "vt_bus_wait",
                                    8192,
                                    nullptr,
                                    4,
                                    nullptr,
                                    1) != pdPASS)
        {
            ESP_LOGE(TAG, "Could not create VT bus wait task");
            vt_view::set_status("ISOBUS startup task failed");
        }
    }
    vt_view::set_bus_state(can_ready, false);

    if (!can_ready)
    {
        vt_view::set_status("CAN start failed");
    }
}
