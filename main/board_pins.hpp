#pragma once

#include "driver/gpio.h"
#include "sdkconfig.h"

namespace board
{
#if CONFIG_ISOVT_BOARD_GUITION_JC8012P4A1C
// Landscape drawing surface; the 800x1280 portrait panel is fed through PPA rotation.
constexpr int LCD_WIDTH = 1280;
constexpr int LCD_HEIGHT = 800;
constexpr int PANEL_NATIVE_WIDTH = 800;
constexpr int PANEL_NATIVE_HEIGHT = 1280;

constexpr gpio_num_t I2C_SDA = GPIO_NUM_7;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_8;
constexpr gpio_num_t TOUCH_IRQ = GPIO_NUM_21;
constexpr gpio_num_t TOUCH_RESET = GPIO_NUM_22;
constexpr gpio_num_t LCD_BACKLIGHT = GPIO_NUM_23;
constexpr gpio_num_t LCD_RESET = GPIO_NUM_27;
#else
constexpr int LCD_WIDTH = 1024;
constexpr int LCD_HEIGHT = 600;

constexpr gpio_num_t I2C_SDA = GPIO_NUM_8;
constexpr gpio_num_t I2C_SCL = GPIO_NUM_9;
constexpr gpio_num_t TOUCH_IRQ = GPIO_NUM_4;

constexpr int EXIO_TOUCH_RESET = 1;
constexpr int EXIO_LCD_BACKLIGHT = 2;
constexpr int EXIO_SD_CS = 4;
constexpr int EXIO_CAN_USB_SELECT = 5;
constexpr int EXIO_LCD_VDD_ENABLE = 6;

constexpr bool EXIO_LEVEL_CAN_MODE = true;
constexpr bool EXIO_LEVEL_USB_MODE = false;
#endif

constexpr gpio_num_t CAN_TX = static_cast<gpio_num_t>(CONFIG_ISOVT_CAN_TX_GPIO);
constexpr gpio_num_t CAN_RX = static_cast<gpio_num_t>(CONFIG_ISOVT_CAN_RX_GPIO);

// The terminal menu and status screens are laid out for 1024x600 and drawn
// scaled by UI_SCALE_NUMERATOR / UI_SCALE_DENOMINATOR, centered on the LCD.
constexpr int UI_WIDTH = 1024;
constexpr int UI_HEIGHT = 600;
#if CONFIG_ISOVT_BOARD_GUITION_JC8012P4A1C
constexpr int UI_SCALE_NUMERATOR = 5;
constexpr int UI_SCALE_DENOMINATOR = 4;
#else
constexpr int UI_SCALE_NUMERATOR = 1;
constexpr int UI_SCALE_DENOMINATOR = 1;
#endif
constexpr int UI_SCREEN_WIDTH = UI_WIDTH * UI_SCALE_NUMERATOR / UI_SCALE_DENOMINATOR;
constexpr int UI_SCREEN_HEIGHT = UI_HEIGHT * UI_SCALE_NUMERATOR / UI_SCALE_DENOMINATOR;
constexpr int UI_X = (LCD_WIDTH - UI_SCREEN_WIDTH) / 2;
constexpr int UI_Y = (LCD_HEIGHT - UI_SCREEN_HEIGHT) / 2;
static_assert((UI_SCREEN_WIDTH <= LCD_WIDTH) && (UI_SCREEN_HEIGHT <= LCD_HEIGHT));
}
