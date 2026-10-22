#pragma once

#include "driver/gpio.h"

namespace board
{
constexpr int LCD_WIDTH = 1024;
constexpr int LCD_HEIGHT = 600;

constexpr gpio_num_t I2C_SDA = GPIO_NUM_8;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_9;
constexpr gpio_num_t TOUCH_IRQ = GPIO_NUM_4;

constexpr gpio_num_t CAN_TX = GPIO_NUM_20;
constexpr gpio_num_t CAN_RX = GPIO_NUM_19;

constexpr int EXIO_TOUCH_RESET = 1;
constexpr int EXIO_LCD_BACKLIGHT = 2;
constexpr int EXIO_SD_CS = 4;
constexpr int EXIO_CAN_USB_SELECT = 5;
constexpr int EXIO_LCD_VDD_ENABLE = 6;

constexpr bool EXIO_LEVEL_CAN_MODE = true;
constexpr bool EXIO_LEVEL_USB_MODE = false;
}
