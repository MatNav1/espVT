# GSL3680 touch driver (vendor copy)

Touch controller driver for the Guition JC8012P4A1C_I_W_Y board (ESP32-P4,
10.1" 800x1280 panel). Copied unchanged from the board vendor's ESP-IDF 6.0.2
demo package (`P4V3X_demo/.../lvgl_sw_rotation/components/esp_lcd_touch_gsl3680`),
the copy that was verified running on the board. Local changes:

- The component manifest names the registry namespace explicitly.
- `esp_lcd_touch_gsl3680_read_data()`: the coordinate buffer holds the full
  44-byte read (the vendor copy used 24 bytes), a failed I2C read returns the
  error and reports no fingers, and the finger count is capped at 10 before
  parsing and at `MAX_FINGER_NUM` before filling `XY_Coordinate`.

## Licensing

- `gsl_point_id.c`: Silead finger tracking algorithm, GNU GPL version 2 or (at
  your option) any later version, see the file header. espVT uses it under
  GPL-3.0-or-later.
- `esp_lcd_touch_gsl3680.c` and `include/`: provided by the board vendor without
  a license header; the header contains the controller firmware table.

espVT is distributed under GPL-3.0-or-later, see `LICENSE` in the repository root.
This component is only built for the ESP32-P4 board.
