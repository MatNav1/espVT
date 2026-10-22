#include "touch_gt911.hpp"

#include "board_pins.hpp"
#include "board_support.hpp"
#include "isobus_terminal_server.hpp"
#include "lvgl_terminal_view.hpp"

#include <array>
#include <cstdint>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr char TAG[] = "gt911";
constexpr std::uint8_t PRIMARY_ADDRESS = 0x5D;
constexpr std::uint8_t BACKUP_ADDRESS = 0x14;
constexpr std::uint16_t PRODUCT_ID_REGISTER = 0x8140;
constexpr std::uint16_t TOUCH_STATUS_REGISTER = 0x814E;
constexpr std::size_t MAX_TOUCH_POINTS = 5;

i2c_master_dev_handle_t device = nullptr;
TaskHandle_t task_handle = nullptr;
std::uint8_t address = 0;

esp_err_t read_register(std::uint16_t register_address, std::uint8_t *data, std::size_t length)
{
    if (device == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const std::uint8_t command[] = {
        static_cast<std::uint8_t>(register_address >> 8),
        static_cast<std::uint8_t>(register_address & 0xFF),
    };
    return i2c_master_transmit_receive(device, command, sizeof(command), data, length, 100);
}

esp_err_t write_register(std::uint16_t register_address, std::uint8_t value)
{
    if (device == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const std::uint8_t command[] = {
        static_cast<std::uint8_t>(register_address >> 8),
        static_cast<std::uint8_t>(register_address & 0xFF),
        value,
    };
    return i2c_master_transmit(device, command, sizeof(command), 100);
}

esp_err_t add_device(std::uint8_t new_address)
{
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = new_address;
    config.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(board::get_i2c_bus(), &config, &device);
}

esp_err_t reset_and_select_primary_address()
{
    gpio_config_t config = {};
    config.mode = GPIO_MODE_OUTPUT;
    config.pin_bit_mask = 1ULL << board::TOUCH_IRQ;
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "GT911 IRQ GPIO setup failed");

    ESP_RETURN_ON_ERROR(board::set_io_expander_output(board::EXIO_TOUCH_RESET, false), TAG, "GT911 reset assert failed");
    gpio_set_level(board::TOUCH_IRQ, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(board::set_io_expander_output(board::EXIO_TOUCH_RESET, true), TAG, "GT911 reset release failed");
    vTaskDelay(pdMS_TO_TICKS(200));

    config = {};
    config.mode = GPIO_MODE_INPUT;
    config.pin_bit_mask = 1ULL << board::TOUCH_IRQ;
    return gpio_config(&config);
}

esp_err_t initialize_controller()
{
    ESP_RETURN_ON_ERROR(board::init_i2c(), TAG, "I2C initialization failed");
    ESP_RETURN_ON_ERROR(reset_and_select_primary_address(), TAG, "GT911 reset failed");

    for (const std::uint8_t candidate_address : {PRIMARY_ADDRESS, BACKUP_ADDRESS})
    {
        ESP_RETURN_ON_ERROR(add_device(candidate_address), TAG, "GT911 I2C device attach failed");

        std::array<std::uint8_t, 4> product_id = {};
        const esp_err_t result = read_register(PRODUCT_ID_REGISTER, product_id.data(), 3);
        if (result == ESP_OK)
        {
            address = candidate_address;
            ESP_LOGI(TAG, "GT911 ready at 0x%02x, product=%c%c%c", address, product_id[0], product_id[1], product_id[2]);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "GT911 did not respond at 0x%02x: %s", candidate_address, esp_err_to_name(result));
        i2c_master_bus_rm_device(device);
        device = nullptr;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t read_points(std::array<touch_gt911::Point, MAX_TOUCH_POINTS> &points, std::size_t &count)
{
    count = 0;
    std::uint8_t status = 0;
    ESP_RETURN_ON_ERROR(read_register(TOUCH_STATUS_REGISTER, &status, 1), TAG, "GT911 status read failed");
    if ((status & 0x80U) == 0U)
    {
        return ESP_OK;
    }

    count = status & 0x0FU;
    if ((count == 0U) || (count > MAX_TOUCH_POINTS))
    {
        count = 0;
        return write_register(TOUCH_STATUS_REGISTER, 0);
    }

    std::array<std::uint8_t, MAX_TOUCH_POINTS * 8> raw = {};
    ESP_RETURN_ON_ERROR(read_register(TOUCH_STATUS_REGISTER + 1, raw.data(), count * 8), TAG, "GT911 points read failed");
    ESP_RETURN_ON_ERROR(write_register(TOUCH_STATUS_REGISTER, 0), TAG, "GT911 status clear failed");

    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint8_t *record = raw.data() + index * 8;
        points[index] = {
            .x = static_cast<std::uint16_t>(record[1] | (record[2] << 8)),
            .y = static_cast<std::uint16_t>(record[3] | (record[4] << 8)),
            .strength = static_cast<std::uint16_t>(record[5] | (record[6] << 8)),
            .id = static_cast<std::uint8_t>(record[0] & 0x0FU),
        };
    }
    return ESP_OK;
}

void touch_task(void *)
{
    std::array<touch_gt911::Point, MAX_TOUCH_POINTS> points = {};
    std::size_t count = 0;
    bool was_touching = false;
    esp_err_t last_error = ESP_OK;

    while (true)
    {
        const esp_err_t result = read_points(points, count);
        if (result != ESP_OK)
        {
            if (result != last_error)
            {
                ESP_LOGW(TAG, "GT911 read failed: %s", esp_err_to_name(result));
            }
            last_error = result;
        }
        else
        {
            last_error = ESP_OK;
            if (count > 0)
            {
                const touch_gt911::Point &point = points[0];
                if (!was_touching)
                {
                    if (!isovt::on_touch_pressed(point.x, point.y))
                    {
                        vt_view::on_touch(point.x, point.y, count);
                    }
                }
                was_touching = true;
            }
            else
            {
                if (was_touching)
                {
                    isovt::on_touch_released();
                }
                was_touching = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}

namespace touch_gt911
{
esp_err_t start()
{
    if (task_handle != nullptr)
    {
        return ESP_OK;
    }

    const esp_err_t result = initialize_controller();
    if (result != ESP_OK)
    {
        return result;
    }

    const BaseType_t task_result = xTaskCreatePinnedToCore(touch_task, "gt911", 4096, nullptr, 4, &task_handle, 0);
    if (task_result != pdPASS)
    {
        task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool is_ready()
{
    return (device != nullptr) && (task_handle != nullptr);
}
}
