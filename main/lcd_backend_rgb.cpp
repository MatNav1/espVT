#include "lcd_backend.hpp"

#include "board_pins.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{
constexpr char TAG[] = "lcd";
constexpr std::size_t FRAME_PIXELS = static_cast<std::size_t>(board::LCD_WIDTH) * board::LCD_HEIGHT;

esp_lcd_panel_handle_t panel = nullptr;
std::array<std::uint16_t *, 2> frame_buffers = {};
SemaphoreHandle_t frame_complete_semaphore = nullptr;
std::size_t drawing_frame_index = 1;

IRAM_ATTR bool on_frame_complete(esp_lcd_panel_handle_t,
                                 const esp_lcd_rgb_panel_event_data_t *,
                                 void *)
{
    if (frame_complete_semaphore == nullptr)
    {
        return false;
    }

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(frame_complete_semaphore, &task_woken);
    return task_woken == pdTRUE;
}

void clear_frame_complete_notification()
{
    while (xSemaphoreTake(frame_complete_semaphore, 0) == pdTRUE)
    {
    }
}
}

namespace lcd_backend
{
esp_err_t init()
{
    if (panel != nullptr)
    {
        return ESP_OK;
    }

    esp_lcd_rgb_panel_config_t config = {};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = 30 * 1000 * 1000;
    config.timings.h_res = board::LCD_WIDTH;
    config.timings.v_res = board::LCD_HEIGHT;
    config.timings.hsync_pulse_width = 162;
    config.timings.hsync_back_porch = 152;
    config.timings.hsync_front_porch = 48;
    config.timings.vsync_pulse_width = 45;
    config.timings.vsync_back_porch = 13;
    config.timings.vsync_front_porch = 3;
    config.timings.flags.pclk_active_neg = true;
    config.data_width = 16;
    config.in_color_format = LCD_COLOR_FMT_RGB565;
    config.num_fbs = 2;
    config.bounce_buffer_size_px = board::LCD_WIDTH * 10;
    config.dma_burst_size = 64;
    config.hsync_gpio_num = GPIO_NUM_46;
    config.vsync_gpio_num = GPIO_NUM_3;
    config.de_gpio_num = GPIO_NUM_5;
    config.pclk_gpio_num = GPIO_NUM_7;
    config.disp_gpio_num = GPIO_NUM_NC;
    const gpio_num_t data_pins[] = {
        GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17,
        GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45,
        GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_1,
        GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
    };
    std::copy(std::begin(data_pins), std::end(data_pins), std::begin(config.data_gpio_nums));
    config.flags.fb_in_psram = true;

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, &panel), TAG, "RGB panel create failed");

    frame_complete_semaphore = xSemaphoreCreateBinary();
    if (frame_complete_semaphore == nullptr)
    {
        ESP_LOGE(TAG, "RGB frame synchronization allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "RGB panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "RGB panel init failed");

    void *first = nullptr;
    void *second = nullptr;
    const esp_err_t result = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &first, &second);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "RGB framebuffer lookup failed: %s", esp_err_to_name(result));
        return result;
    }

    frame_buffers[0] = static_cast<std::uint16_t *>(first);
    frame_buffers[1] = static_cast<std::uint16_t *>(second);

    std::fill_n(frame_buffers[0], FRAME_PIXELS, 0x0000);
    std::fill_n(frame_buffers[1], FRAME_PIXELS, 0x0000);

    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_frame_buf_complete = on_frame_complete;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr), TAG,
                        "RGB frame callback registration failed");

    ESP_LOGI(TAG, "RGB LCD ready: %dx%d, 30 MHz, double RGB565 framebuffer with bounce DMA",
             board::LCD_WIDTH, board::LCD_HEIGHT);
    return ESP_OK;
}

std::uint16_t *draw_buffer()
{
    return frame_buffers[drawing_frame_index];
}

esp_err_t present()
{
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel,
                                                 0,
                                                 0,
                                                 board::LCD_WIDTH,
                                                 board::LCD_HEIGHT,
                                                 frame_buffers[drawing_frame_index]);
    if (result == ESP_OK)
    {
        // Wait for a complete bounce-buffer frame after requesting the buffer switch.
        clear_frame_complete_notification();
        if (xSemaphoreTake(frame_complete_semaphore, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "RGB frame completion timed out");
            result = ESP_ERR_TIMEOUT;
        }
    }
    if (result == ESP_OK)
    {
        drawing_frame_index ^= 1U;
    }
    return result;
}
}
