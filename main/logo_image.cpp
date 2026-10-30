#include "logo_image.hpp"

#include "board_pins.hpp"
#include "lcd_panel.hpp"
#include "logo_rgb565.hpp"

namespace logo_image
{
int height()
{
    return logo_asset::HEIGHT * board::UI_SCALE_DENOMINATOR / board::UI_SCALE_NUMERATOR;
}

void draw_centered(int top_y)
{
    // The asset is generated at the UI scale, so it is copied 1:1 to the LCD.
    const int width = logo_asset::WIDTH * board::UI_SCALE_DENOMINATOR / board::UI_SCALE_NUMERATOR;
    const int left = (board::UI_WIDTH - width) / 2;
    lcd_panel::draw_rgb565(left, top_y, width, height(), logo_asset::PIXELS, logo_asset::WIDTH, logo_asset::HEIGHT);
}
}
