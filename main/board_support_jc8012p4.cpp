#include "board_support.hpp"

#include "board_pins.hpp"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

namespace
{
constexpr char TAG[] = "board";

i2c_master_bus_handle_t bus = nullptr;
}

namespace board
{
esp_err_t init_i2c()
{
    if (bus != nullptr)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t config = {};
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = I2C_SDA;
    config.scl_io_num = I2C_SCL;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    const esp_err_t result = i2c_new_master_bus(&config, &bus);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "I2C initialized on SDA=%d SCL=%d", I2C_SDA, I2C_SCL);
    }
    return result;
}

i2c_master_bus_handle_t get_i2c_bus()
{
    return bus;
}

esp_err_t init_io_expander()
{
    // The JC8012 board has no IO expander. Drive the external transceiver's TX
    // recessive until TWAI takes over the pin.
    ESP_RETURN_ON_ERROR(gpio_reset_pin(CAN_TX), TAG, "CAN TX reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(CAN_TX, GPIO_MODE_OUTPUT), TAG, "CAN TX direction failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(CAN_TX, 1), TAG, "CAN TX recessive level failed");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(CAN_RX), TAG, "CAN RX reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(CAN_RX, GPIO_MODE_INPUT), TAG, "CAN RX direction failed");
    return ESP_OK;
}

esp_err_t set_io_expander_output(int, bool)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t enable_can_mode()
{
    ESP_LOGI(TAG, "CAN path ready: TX=%d RX=%d", gpio_get_level(CAN_TX), gpio_get_level(CAN_RX));
    return ESP_OK;
}

esp_err_t recover_can_mode()
{
    return enable_can_mode();
}

esp_err_t enable_lcd_power()
{
    // Panel power is always on; the backlight is switched by lcd_panel once the
    // first frame is ready.
    return ESP_OK;
}
}
