#include "board_support.hpp"

#include "board_pins.hpp"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr char TAG[] = "board";
constexpr uint8_t IO_EXPANDER_ADDR = 0x24;
constexpr uint8_t IO_EXPANDER_DIRECTION_REGISTER = 0x02;
constexpr uint8_t IO_EXPANDER_OUTPUT_REGISTER = 0x03;
constexpr uint8_t IO_EXPANDER_ALL_OUTPUTS = 0xFF;

i2c_master_bus_handle_t bus = nullptr;
i2c_master_dev_handle_t ioexp = nullptr;
// Select CAN in the first expander write. GPIO20 is driven recessive immediately
// before that write so a cold boot cannot assert a dominant bus state.
uint8_t output_shadow = IO_EXPANDER_ALL_OUTPUTS;

esp_err_t write_io_expander_register(uint8_t reg, uint8_t value)
{
    if (ioexp == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(ioexp, data, sizeof(data), 100);
}

esp_err_t read_io_expander_register(uint8_t reg, uint8_t &value)
{
    if (ioexp == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(ioexp, &reg, 1, &value, 1, 100);
}

esp_err_t write_io_expander(uint8_t value)
{
    output_shadow = value;
    return write_io_expander_register(IO_EXPANDER_OUTPUT_REGISTER, output_shadow);
}
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
    ESP_RETURN_ON_ERROR(init_i2c(), TAG, "I2C init failed");
    if (ioexp != nullptr)
    {
        return ESP_OK;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = IO_EXPANDER_ADDR;
    config.scl_speed_hz = 400000;

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &ioexp), TAG, "IO expander attach failed");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(CAN_TX), TAG, "CAN TX reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(CAN_TX, GPIO_MODE_OUTPUT), TAG, "CAN TX direction failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(CAN_TX, 1), TAG, "CAN TX recessive level failed");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(CAN_RX), TAG, "CAN RX reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(CAN_RX, GPIO_MODE_INPUT), TAG, "CAN RX direction failed");
    esp_rom_delay_us(2000);
    ESP_RETURN_ON_ERROR(write_io_expander_register(IO_EXPANDER_DIRECTION_REGISTER, IO_EXPANDER_ALL_OUTPUTS), TAG, "IO expander mode write failed");
    // The Waveshare IO-extension MCU needs time to apply its mode after a cold
    // power-on. Writes sent immediately after register 0x02 can be acknowledged
    // over I2C without the physical outputs having switched yet.
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(write_io_expander(output_shadow), TAG, "IO expander write failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "IO expander initialized at 0x%02x", IO_EXPANDER_ADDR);
    return ESP_OK;
}

esp_err_t set_io_expander_output(int pin, bool high)
{
    if ((pin < 0) || (pin > 7))
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(init_io_expander(), TAG, "IO expander init failed");

    uint8_t next = output_shadow;
    if (high)
    {
        next |= static_cast<uint8_t>(1U << pin);
    }
    else
    {
        next &= static_cast<uint8_t>(~(1U << pin));
    }
    return write_io_expander(next);
}

esp_err_t enable_can_mode()
{
    ESP_LOGI(TAG, "Switching shared USB/CAN lines to CAN mode");

    bool selection_verified = false;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        ESP_RETURN_ON_ERROR(set_io_expander_output(EXIO_CAN_USB_SELECT, EXIO_LEVEL_CAN_MODE),
                            TAG,
                            "CAN mode select write failed");
        esp_rom_delay_us(20000);

        uint8_t output_value = 0;
        const esp_err_t read_result = read_io_expander_register(IO_EXPANDER_OUTPUT_REGISTER, output_value);
        if (read_result != ESP_OK)
        {
            ESP_LOGW(TAG, "CAN selector readback attempt %d failed: %s",
                     attempt,
                     esp_err_to_name(read_result));
            continue;
        }

        const bool selected = ((output_value >> EXIO_CAN_USB_SELECT) & 1U) != 0;
        ESP_LOGI(TAG, "IO expander output=0x%02x (CAN select=%u, attempt=%d)",
                 output_value,
                 static_cast<unsigned>(selected),
                 attempt);
        if (selected == EXIO_LEVEL_CAN_MODE)
        {
            selection_verified = true;
            break;
        }
        ESP_LOGW(TAG, "CAN selector did not latch; repeating write");
    }

    if (!selection_verified)
    {
        ESP_LOGW(TAG, "CAN selector readback was not confirmed; output remains forced to CAN");
    }
    ESP_LOGI(TAG, "CAN path ready: TX=%d RX=%d", gpio_get_level(CAN_TX), gpio_get_level(CAN_RX));
    return ESP_OK;
}

esp_err_t recover_can_mode()
{
    // Keep the transceiver connected. TWAI bus-off recovery is handled by the
    // controller and must not toggle the shared CAN/USB selector.
    return enable_can_mode();
}

esp_err_t enable_lcd_power()
{
    ESP_RETURN_ON_ERROR(set_io_expander_output(EXIO_LCD_VDD_ENABLE, true), TAG, "LCD VDD enable failed");
    ESP_RETURN_ON_ERROR(set_io_expander_output(EXIO_LCD_BACKLIGHT, true), TAG, "LCD backlight enable failed");
    ESP_RETURN_ON_ERROR(set_io_expander_output(EXIO_TOUCH_RESET, true), TAG, "Touch reset release failed");
    return ESP_OK;
}
}
