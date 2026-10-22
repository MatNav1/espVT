#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace board
{
esp_err_t init_i2c();
i2c_master_bus_handle_t get_i2c_bus();
esp_err_t init_io_expander();
esp_err_t set_io_expander_output(int pin, bool high);
esp_err_t enable_can_mode();
esp_err_t recover_can_mode();
esp_err_t enable_lcd_power();
}
