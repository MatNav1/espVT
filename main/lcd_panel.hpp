#pragma once

#include <cstdint>

#include "esp_err.h"

namespace lcd_panel
{
enum class TextEncoding : std::uint8_t
{
    ISO8859_1 = 0,
    ISO8859_15 = 1,
    ISO8859_2 = 2,
    ISO8859_4 = 4,
    ISO8859_5 = 5,
    ISO8859_7 = 7,
    UTF8 = 0xFF, // terminal UI texts
};

esp_err_t init();
bool is_ready();
esp_err_t begin_frame();
esp_err_t present();
void set_view_transform(int offset_x, int offset_y, int scale_numerator, int scale_denominator);
void set_view_transform_xy(int offset_x,
                           int offset_y,
                           int scale_x_numerator,
                           int scale_x_denominator,
                           int scale_y_numerator,
                           int scale_y_denominator);
void reset_view_transform();
void set_clip_rect(int x, int y, int width, int height);
void reset_clip_rect();
// All coordinates are relative to the current drawing area. begin_frame()
// selects the whole LCD; use_ui_area() selects the centered UI_WIDTH x UI_HEIGHT
// window the menu and status screens are laid out for, scaled by
// board::UI_SCALE_NUMERATOR / UI_SCALE_DENOMINATOR.
void use_full_screen();
void use_ui_area();
// LCD position of a point in the current drawing area (without view transform).
int to_lcd_x(int x);
int to_lcd_y(int y);
esp_err_t fill(std::uint16_t color);
esp_err_t fill_rect(int x, int y, int width, int height, std::uint16_t color);
esp_err_t draw_rgb565(int x, int y, int width, int height, const std::uint16_t *pixels,
                      int source_width = 0, int source_height = 0);
void draw_line(int x0, int y0, int x1, int y1, std::uint16_t color, int thickness = 1);
// UTF-8 UI text in 6x10 cells per scale step.
void draw_text(int x, int y, int scale, const char *text, std::uint16_t color);
// Number of characters in a UTF-8 UI text.
int text_length(const char *text);
void draw_text_sized(int x,
                     int y,
                     int character_width,
                     int character_height,
                     const char *text,
                     std::uint16_t color,
                     bool bold = false,
                     bool underlined = false,
                     bool crossed_out = false,
                     TextEncoding encoding = TextEncoding::ISO8859_1);
}
