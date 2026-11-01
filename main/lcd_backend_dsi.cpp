#include "lcd_backend.hpp"

#include "board_pins.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern "C"
{
extern const jd9365_lcd_init_cmd_t jc8012_lcd_init_cmds[];
extern const size_t jc8012_lcd_init_cmds_count;
}

namespace
{
constexpr char TAG[] = "lcd";
constexpr std::size_t FRAME_PIXELS = static_cast<std::size_t>(board::LCD_WIDTH) * board::LCD_HEIGHT;
constexpr std::size_t FRAME_BYTES = FRAME_PIXELS * sizeof(std::uint16_t);
// MIPI-DSI parameters of the vendor demo verified on this board.
constexpr int DSI_LANE_COUNT = 2;
constexpr int DSI_LANE_BITRATE_MBPS = 1500;
constexpr int DPI_CLOCK_MHZ = 70;
constexpr int DSI_PHY_LDO_CHANNEL = 3;
constexpr int DSI_PHY_LDO_MILLIVOLTS = 2500;
// PSRAM buffers are written by the CPU and read by DMA; align to the L2 cache line.
constexpr std::size_t PSRAM_ALIGNMENT = 128;

esp_ldo_channel_handle_t dsi_phy_power = nullptr;
esp_lcd_panel_handle_t panel = nullptr;
ppa_client_handle_t rotator = nullptr;
std::uint16_t *landscape_buffer = nullptr;
std::array<std::uint16_t *, 2> panel_buffers = {};
std::size_t back_buffer_index = 1;
SemaphoreHandle_t frame_complete_semaphore = nullptr;

IRAM_ATTR bool on_frame_complete(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *)
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

esp_err_t create_panel()
{
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = DSI_PHY_LDO_CHANNEL;
    ldo_config.voltage_mv = DSI_PHY_LDO_MILLIVOLTS;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power), TAG, "DSI PHY power failed");

    esp_lcd_dsi_bus_config_t bus_config = {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = DSI_LANE_COUNT;
    bus_config.lane_bit_rate_mbps = DSI_LANE_BITRATE_MBPS;
    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus), TAG, "DSI bus create failed");

    esp_lcd_dbi_io_config_t io_config = {};
    io_config.virtual_channel = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi_bus, &io_config, &io), TAG, "DSI command IO create failed");

    esp_lcd_dpi_panel_config_t dpi_config = {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = DPI_CLOCK_MHZ;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = panel_buffers.size();
    dpi_config.video_timing.h_size = board::PANEL_NATIVE_WIDTH;
    dpi_config.video_timing.v_size = board::PANEL_NATIVE_HEIGHT;
    dpi_config.video_timing.hsync_pulse_width = 20;
    dpi_config.video_timing.hsync_back_porch = 20;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_pulse_width = 4;
    dpi_config.video_timing.vsync_back_porch = 10;
    dpi_config.video_timing.vsync_front_porch = 30;

    jd9365_vendor_config_t vendor_config = {};
    vendor_config.init_cmds = jc8012_lcd_init_cmds;
    vendor_config.init_cmds_size = static_cast<std::uint16_t>(jc8012_lcd_init_cmds_count);
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.mipi_config.lane_num = DSI_LANE_COUNT;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = board::LCD_RESET;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9365(io, &panel_config, &panel), TAG, "JD9365 panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "JD9365 reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "JD9365 init failed");
    return esp_lcd_panel_disp_on_off(panel, true);
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

    ESP_RETURN_ON_ERROR(create_panel(), TAG, "MIPI-DSI panel init failed");

    void *first = nullptr;
    void *second = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(panel, 2, &first, &second), TAG,
                        "DPI framebuffer lookup failed");
    panel_buffers[0] = static_cast<std::uint16_t *>(first);
    panel_buffers[1] = static_cast<std::uint16_t *>(second);
    for (auto *buffer : panel_buffers)
    {
        std::fill_n(buffer, FRAME_PIXELS, 0x0000);
        // Writes the cleared buffer back from the cache and makes it the visible one.
        ESP_RETURN_ON_ERROR(esp_lcd_panel_draw_bitmap(panel,
                                                      0,
                                                      0,
                                                      board::PANEL_NATIVE_WIDTH,
                                                      board::PANEL_NATIVE_HEIGHT,
                                                      buffer),
                            TAG,
                            "DPI framebuffer clear failed");
    }
    back_buffer_index = 0;

    landscape_buffer = static_cast<std::uint16_t *>(
        heap_caps_aligned_calloc(PSRAM_ALIGNMENT, 1, FRAME_BYTES, MALLOC_CAP_SPIRAM));
    frame_complete_semaphore = xSemaphoreCreateBinary();
    if ((landscape_buffer == nullptr) || (frame_complete_semaphore == nullptr))
    {
        ESP_LOGE(TAG, "LCD frame buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ppa_client_config_t ppa_config = {};
    ppa_config.oper_type = PPA_OPERATION_SRM;
    ppa_config.max_pending_trans_num = 1;
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_config, &rotator), TAG, "PPA client registration failed");

    esp_lcd_dpi_panel_event_callbacks_t callbacks = {};
    callbacks.on_frame_buf_complete = on_frame_complete;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, nullptr), TAG,
                        "DPI frame callback registration failed");

    gpio_config_t backlight_config = {};
    backlight_config.pin_bit_mask = 1ULL << board::LCD_BACKLIGHT;
    backlight_config.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "Backlight GPIO config failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(board::LCD_BACKLIGHT, 1), TAG, "Backlight enable failed");

    ESP_LOGI(TAG, "MIPI-DSI LCD ready: %dx%d landscape on %dx%d JD9365, PPA rotation",
             board::LCD_WIDTH, board::LCD_HEIGHT, board::PANEL_NATIVE_WIDTH, board::PANEL_NATIVE_HEIGHT);
    return ESP_OK;
}

std::uint16_t *draw_buffer()
{
    return landscape_buffer;
}

esp_err_t present()
{
    std::uint16_t *target = panel_buffers[back_buffer_index];

    ppa_srm_oper_config_t rotation = {};
    rotation.in.buffer = landscape_buffer;
    rotation.in.pic_w = board::LCD_WIDTH;
    rotation.in.pic_h = board::LCD_HEIGHT;
    rotation.in.block_w = board::LCD_WIDTH;
    rotation.in.block_h = board::LCD_HEIGHT;
    rotation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    rotation.out.buffer = target;
    rotation.out.buffer_size = FRAME_BYTES;
    rotation.out.pic_w = board::PANEL_NATIVE_WIDTH;
    rotation.out.pic_h = board::PANEL_NATIVE_HEIGHT;
    rotation.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#if CONFIG_ISOVT_DISPLAY_ROTATION_270
    rotation.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
#else
    rotation.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
#endif
    rotation.scale_x = 1.0F;
    rotation.scale_y = 1.0F;
    rotation.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t result = ppa_do_scale_rotate_mirror(rotator, &rotation);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "PPA rotation failed: %s", esp_err_to_name(result));
        return result;
    }

    clear_frame_complete_notification();
    result = esp_lcd_panel_draw_bitmap(panel, 0, 0, board::PANEL_NATIVE_WIDTH, board::PANEL_NATIVE_HEIGHT, target);
    if (result == ESP_OK)
    {
        // The callback fires once the DMA has moved on to the new buffer, which
        // frees the previous one for the next rotation.
        if (xSemaphoreTake(frame_complete_semaphore, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "DPI frame completion timed out");
            result = ESP_ERR_TIMEOUT;
        }
    }
    if (result == ESP_OK)
    {
        back_buffer_index ^= 1U;
    }
    return result;
}
}
