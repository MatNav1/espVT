#include "logo_image.hpp"

#include "board_pins.hpp"
#include "lcd_panel.hpp"
#include "logo_rgb565.hpp"

namespace logo_image
{
int height()
{
    return logo_asset::HEIGHT;
}

void draw_centered(int top_y)
{
    const int left = (board::LCD_WIDTH - logo_asset::WIDTH) / 2;
    lcd_panel::draw_rgb565(left, top_y, logo_asset::WIDTH, logo_asset::HEIGHT, logo_asset::PIXELS);
}
}
