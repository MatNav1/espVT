#include "lcd_panel.hpp"

#include "board_pins.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdlib>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace
{
constexpr char TAG[] = "lcd";
constexpr std::size_t FRAME_PIXELS = static_cast<std::size_t>(board::LCD_WIDTH) * board::LCD_HEIGHT;

extern "C"
{
extern const std::uint8_t vt_font_asset_start[] asm("_binary_vt_font_roboto_mono_bin_start");
extern const std::uint8_t vt_font_asset_end[] asm("_binary_vt_font_roboto_mono_bin_end");
}

esp_lcd_panel_handle_t panel = nullptr;
std::array<std::uint16_t *, 2> frame_buffers = {};
SemaphoreHandle_t frame_mutex = nullptr;
SemaphoreHandle_t frame_complete_semaphore = nullptr;
std::size_t drawing_frame_index = 1;
bool frame_in_progress = false;

struct ViewTransform
{
    int offset_x = 0;
    int offset_y = 0;
    int scale_numerator = 1;
    int scale_denominator = 1;
    int scale_y_numerator = 1;
    int scale_y_denominator = 1;
    int clip_x = 0;
    int clip_y = 0;
    int clip_width = board::LCD_WIDTH;
    int clip_height = board::LCD_HEIGHT;
};

ViewTransform view_transform;

struct FontAssetSize
{
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
};

std::uint16_t read_u16(const std::uint8_t *data)
{
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t *data)
{
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool font_asset_valid()
{
    return ((vt_font_asset_end - vt_font_asset_start) >= 12) &&
           (std::memcmp(vt_font_asset_start, "VTF1", 4) == 0) &&
           (read_u16(vt_font_asset_start + 4) == 1);
}

int font_glyph_index(std::uint16_t codepoint)
{
    if (!font_asset_valid())
    {
        return -1;
    }
    const int glyph_count = read_u16(vt_font_asset_start + 6);
    const std::uint8_t *codepoints = vt_font_asset_start + 12;
    int low = 0;
    int high = glyph_count - 1;
    while (low <= high)
    {
        const int middle = low + (high - low) / 2;
        const std::uint16_t value = read_u16(codepoints + middle * 2);
        if (value == codepoint)
        {
            return middle;
        }
        if (value < codepoint)
        {
            low = middle + 1;
        }
        else
        {
            high = middle - 1;
        }
    }
    return -1;
}

bool select_font_size(int requested_width, int requested_height, FontAssetSize &selected)
{
    if (!font_asset_valid())
    {
        return false;
    }
    const int glyph_count = read_u16(vt_font_asset_start + 6);
    const int size_count = read_u16(vt_font_asset_start + 8);
    const std::uint8_t *entries = vt_font_asset_start + 12 + glyph_count * 2;
    int best_score = 0x7FFFFFFF;
    for (int index = 0; index < size_count; ++index)
    {
        const std::uint8_t *entry = entries + index * 12;
        FontAssetSize candidate = {
            .width = read_u16(entry),
            .height = read_u16(entry + 2),
            .offset = read_u32(entry + 4),
            .stride = read_u32(entry + 8),
        };
        const int score = std::abs(static_cast<int>(candidate.width) - requested_width) * 2 +
                          std::abs(static_cast<int>(candidate.height) - requested_height);
        if (score < best_score)
        {
            best_score = score;
            selected = candidate;
        }
    }
    return (selected.width > 0) &&
           (selected.height > 0) &&
           (selected.offset + selected.stride <= static_cast<std::uint32_t>(vt_font_asset_end - vt_font_asset_start));
}

std::uint16_t decode_character(std::uint8_t value, lcd_panel::TextEncoding encoding)
{
    static constexpr std::uint16_t ISO_8859_2_HIGH[96] = {
        0x00A0, 0x0104, 0x02D8, 0x0141, 0x00A4, 0x013D, 0x015A, 0x00A7,
        0x00A8, 0x0160, 0x015E, 0x0164, 0x0179, 0x00AD, 0x017D, 0x017B,
        0x00B0, 0x0105, 0x02DB, 0x0142, 0x00B4, 0x013E, 0x015B, 0x02C7,
        0x00B8, 0x0161, 0x015F, 0x0165, 0x017A, 0x02DD, 0x017E, 0x017C,
        0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
        0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
        0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
        0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
        0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
        0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
        0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
        0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
    };

    if (value < 0x80)
    {
        return value;
    }
    if (value < 0xA0)
    {
        return '?';
    }
    if (encoding == lcd_panel::TextEncoding::ISO8859_2)
    {
        return ISO_8859_2_HIGH[value - 0xA0];
    }
    if (encoding == lcd_panel::TextEncoding::ISO8859_15)
    {
        switch (value)
        {
            case 0xA4: return 0x20AC;
            case 0xA6: return 0x0160;
            case 0xA8: return 0x0161;
            case 0xB4: return 0x017D;
            case 0xB8: return 0x017E;
            case 0xBC: return 0x0152;
            case 0xBD: return 0x0153;
            case 0xBE: return 0x0178;
            default: break;
        }
    }
    return value;
}

int transform_x(int x)
{
    return view_transform.offset_x + (x * view_transform.scale_numerator) / view_transform.scale_denominator;
}

int transform_y(int y)
{
    return view_transform.offset_y + (y * view_transform.scale_y_numerator) / view_transform.scale_y_denominator;
}

IRAM_ATTR bool on_frame_complete(esp_lcd_panel_handle_t,
                                 const esp_lcd_rgb_panel_event_data_t *,
                                 void *)
{
    if (frame_complete_semaphore == nullptr)
    {
        return false;
    }

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(frame_complete_semaphore, &task_woken);
    return task_woken == pdTRUE;
}

void clear_frame_complete_notification()
{
    while (xSemaphoreTake(frame_complete_semaphore, 0) == pdTRUE)
    {
    }
}

const std::uint8_t *glyph_for(char character)
{
    static constexpr std::uint8_t ALPHABET[][5] = {
        {0x7E, 0x09, 0x09, 0x7E, 0x00}, // A
        {0x7F, 0x49, 0x49, 0x36, 0x00}, // B
        {0x3E, 0x41, 0x41, 0x22, 0x00}, // C
        {0x7F, 0x41, 0x41, 0x3E, 0x00}, // D
        {0x7F, 0x49, 0x49, 0x41, 0x00}, // E
        {0x7F, 0x09, 0x09, 0x01, 0x00}, // F
        {0x3E, 0x41, 0x49, 0x7A, 0x00}, // G
        {0x7F, 0x08, 0x08, 0x7F, 0x00}, // H
        {0x41, 0x7F, 0x41, 0x00, 0x00}, // I
        {0x20, 0x40, 0x41, 0x3F, 0x00}, // J
        {0x7F, 0x08, 0x14, 0x63, 0x00}, // K
        {0x7F, 0x40, 0x40, 0x40, 0x00}, // L
        {0x7F, 0x02, 0x04, 0x02, 0x7F}, // M
        {0x7F, 0x06, 0x18, 0x7F, 0x00}, // N
        {0x3E, 0x41, 0x41, 0x3E, 0x00}, // O
        {0x7F, 0x09, 0x09, 0x06, 0x00}, // P
        {0x3E, 0x41, 0x61, 0x7E, 0x00}, // Q
        {0x7F, 0x09, 0x19, 0x66, 0x00}, // R
        {0x26, 0x49, 0x49, 0x32, 0x00}, // S
        {0x01, 0x7F, 0x01, 0x00, 0x00}, // T
        {0x3F, 0x40, 0x40, 0x3F, 0x00}, // U
        {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
        {0x7F, 0x20, 0x18, 0x20, 0x7F}, // W
        {0x63, 0x14, 0x08, 0x14, 0x63}, // X
        {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
        {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    };
    static constexpr std::uint8_t DIGITS[][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
        {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
        {0x62, 0x51, 0x49, 0x49, 0x46}, // 2
        {0x22, 0x41, 0x49, 0x49, 0x36}, // 3
        {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
        {0x2F, 0x49, 0x49, 0x49, 0x31}, // 5
        {0x3E, 0x49, 0x49, 0x49, 0x32}, // 6
        {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
        {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
        {0x26, 0x49, 0x49, 0x49, 0x3E}, // 9
    };
    static constexpr std::uint8_t SPACE[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr std::uint8_t HYPHEN[] = {0x08, 0x08, 0x08, 0x08, 0x00};
    static constexpr std::uint8_t COLON[] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static constexpr std::uint8_t SLASH[] = {0x60, 0x18, 0x06, 0x00, 0x00};
    static constexpr std::uint8_t PERCENT[] = {0x23, 0x13, 0x08, 0x64, 0x62};
    static constexpr std::uint8_t PERIOD[] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static constexpr std::uint8_t COMMA[] = {0x00, 0x40, 0x20, 0x00, 0x00};
    static constexpr std::uint8_t UNDERSCORE[] = {0x40, 0x40, 0x40, 0x40, 0x40};
    static constexpr std::uint8_t PLUS[] = {0x08, 0x08, 0x3E, 0x08, 0x08};
    static constexpr std::uint8_t EQUALS[] = {0x14, 0x14, 0x14, 0x14, 0x00};
    static constexpr std::uint8_t EXCLAMATION[] = {0x00, 0x00, 0x5F, 0x00, 0x00};
    static constexpr std::uint8_t QUESTION[] = {0x02, 0x01, 0x51, 0x09, 0x06};
    static constexpr std::uint8_t OPEN_PAREN[] = {0x00, 0x1C, 0x22, 0x41, 0x00};
    static constexpr std::uint8_t CLOSE_PAREN[] = {0x00, 0x41, 0x22, 0x1C, 0x00};
    static constexpr std::uint8_t OPEN_BRACKET[] = {0x00, 0x7F, 0x41, 0x41, 0x00};
    static constexpr std::uint8_t CLOSE_BRACKET[] = {0x00, 0x41, 0x41, 0x7F, 0x00};
    static constexpr std::uint8_t LESS_THAN[] = {0x08, 0x14, 0x22, 0x41, 0x00};
    static constexpr std::uint8_t GREATER_THAN[] = {0x41, 0x22, 0x14, 0x08, 0x00};
    static constexpr std::uint8_t UNKNOWN[] = {0x02, 0x01, 0x59, 0x05, 0x02};

    if ((character >= 'A') && (character <= 'Z'))
    {
        return ALPHABET[character - 'A'];
    }
    if ((character >= '0') && (character <= '9'))
    {
        return DIGITS[character - '0'];
    }
    switch (character)
    {
        case ' ':
            return SPACE;
        case '-':
            return HYPHEN;
        case ':':
            return COLON;
        case '/':
            return SLASH;
        case '%':
            return PERCENT;
        case '.':
            return PERIOD;
        case ',':
            return COMMA;
        case '_':
            return UNDERSCORE;
        case '+':
            return PLUS;
        case '=':
            return EQUALS;
        case '!':
            return EXCLAMATION;
        case '?':
            return QUESTION;
        case '(':
            return OPEN_PAREN;
        case ')':
            return CLOSE_PAREN;
        case '[':
            return OPEN_BRACKET;
        case ']':
            return CLOSE_BRACKET;
        case '<':
            return LESS_THAN;
        case '>':
            return GREATER_THAN;
        default:
            return UNKNOWN;
    }
}

void write_pixel(int x, int y, std::uint16_t color)
{
    if ((x < view_transform.clip_x) || (x >= view_transform.clip_x + view_transform.clip_width) ||
        (y < view_transform.clip_y) || (y >= view_transform.clip_y + view_transform.clip_height) ||
        (x < 0) || (x >= board::LCD_WIDTH) || (y < 0) || (y >= board::LCD_HEIGHT))
    {
        return;
    }

    const std::size_t offset = static_cast<std::size_t>(y) * board::LCD_WIDTH + x;
    frame_buffers[drawing_frame_index][offset] = color;
}

std::uint16_t blend_rgb565(std::uint16_t background, std::uint16_t foreground, std::uint8_t alpha)
{
    if (alpha == 0)
    {
        return background;
    }
    if (alpha >= 3)
    {
        return foreground;
    }
    const int inverse = 3 - alpha;
    const int red = (((background >> 11U) & 0x1FU) * inverse + ((foreground >> 11U) & 0x1FU) * alpha + 1) / 3;
    const int green = (((background >> 5U) & 0x3FU) * inverse + ((foreground >> 5U) & 0x3FU) * alpha + 1) / 3;
    const int blue = ((background & 0x1FU) * inverse + (foreground & 0x1FU) * alpha + 1) / 3;
    return static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
}

void fill_rgb565(std::uint16_t *destination, std::size_t count, std::uint16_t color)
{
    if ((destination == nullptr) || (count == 0))
    {
        return;
    }
    if ((reinterpret_cast<std::uintptr_t>(destination) & 0x03U) != 0U)
    {
        *destination++ = color;
        --count;
    }
    const std::uint32_t packed = static_cast<std::uint32_t>(color) |
                                 (static_cast<std::uint32_t>(color) << 16U);
    auto *wide_destination = reinterpret_cast<std::uint32_t *>(destination);
    std::fill_n(wide_destination, count / 2U, packed);
    if ((count & 1U) != 0U)
    {
        destination[count - 1U] = color;
    }
}

bool draw_asset_glyph(int x,
                      int y,
                      int character_width,
                      int character_height,
                      std::uint16_t codepoint,
                      std::uint16_t color,
                      bool bold,
                      const FontAssetSize &font_size)
{
    const int glyph_index = font_glyph_index(codepoint);
    if (glyph_index < 0)
    {
        return false;
    }

    const std::uint64_t glyph_offset = static_cast<std::uint64_t>(font_size.offset) +
                                       static_cast<std::uint64_t>(glyph_index) * font_size.stride;
    if (glyph_offset + font_size.stride > static_cast<std::uint64_t>(vt_font_asset_end - vt_font_asset_start))
    {
        return false;
    }
    const std::uint8_t *glyph = vt_font_asset_start + glyph_offset;
    for (int row = 0; row < character_height; ++row)
    {
        const int physical_top = std::max({ 0, transform_y(y + row), view_transform.clip_y });
        const int physical_bottom = std::min({ board::LCD_HEIGHT,
                                               transform_y(y + row + 1),
                                               view_transform.clip_y + view_transform.clip_height });
        if (physical_top >= physical_bottom)
        {
            continue;
        }
        const int source_y = std::min<int>(font_size.height - 1, row * font_size.height / character_height);
        for (int column = 0; column < character_width; ++column)
        {
            const int source_x = std::min<int>(font_size.width - 1, column * font_size.width / character_width);
            const int source_index = source_y * font_size.width + source_x;
            const std::uint8_t alpha = static_cast<std::uint8_t>((glyph[source_index / 4] >> ((source_index % 4) * 2)) & 0x03U);
            if (alpha == 0)
            {
                continue;
            }
            const int logical_right = x + column + ((bold && (column + 1 < character_width)) ? 2 : 1);
            const int physical_left = std::max({ 0, transform_x(x + column), view_transform.clip_x });
            const int physical_right = std::min({ board::LCD_WIDTH,
                                                  transform_x(logical_right),
                                                  view_transform.clip_x + view_transform.clip_width });
            if (physical_left >= physical_right)
            {
                continue;
            }
            for (int physical_y = physical_top; physical_y < physical_bottom; ++physical_y)
            {
                auto *destination = frame_buffers[drawing_frame_index] +
                                    static_cast<std::size_t>(physical_y) * board::LCD_WIDTH + physical_left;
                if (alpha >= 3)
                {
                    std::fill_n(destination, physical_right - physical_left, color);
                }
                else
                {
                    for (int physical_x = physical_left; physical_x < physical_right; ++physical_x)
                    {
                        *destination = blend_rgb565(*destination, color, alpha);
                        ++destination;
                    }
                }
            }
        }
    }
    return true;
}

esp_err_t fill_rect_raw(int x, int y, int width, int height, std::uint16_t color)
{
    if (!lcd_panel::is_ready() || !frame_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((width <= 0) || (height <= 0))
    {
        return ESP_OK;
    }

    const int x_start = std::max({0, x, view_transform.clip_x});
    const int y_start = std::max({0, y, view_transform.clip_y});
    const int x_end = std::min({board::LCD_WIDTH, x + width, view_transform.clip_x + view_transform.clip_width});
    const int y_end = std::min({board::LCD_HEIGHT, y + height, view_transform.clip_y + view_transform.clip_height});
    if ((x_start >= x_end) || (y_start >= y_end))
    {
        return ESP_OK;
    }

    for (int row = y_start; row < y_end; ++row)
    {
        std::uint16_t *line = frame_buffers[drawing_frame_index] + static_cast<std::size_t>(row) * board::LCD_WIDTH + x_start;
        fill_rgb565(line, static_cast<std::size_t>(x_end - x_start), color);
    }
    return ESP_OK;
}
}

namespace lcd_panel
{
esp_err_t init()
{
    if (panel != nullptr)
    {
        return ESP_OK;
    }

    esp_lcd_rgb_panel_config_t config = {};
    config.clk_src = LCD_CLK_SRC_DEFAULT;
    config.timings.pclk_hz = 30 * 1000 * 1000;
    config.timings.h_res = board::LCD_WIDTH;
    config.timings.v_res = board::LCD_HEIGHT;
    config.timings.hsync_pulse_width = 162;
    config.timings.hsync_back_porch = 152;
    config.timings.hsync_front_porch = 48;
    config.timings.vsync_pulse_width = 45;
    config.timings.vsync_back_porch = 13;
    config.timings.vsync_front_porch = 3;
    config.timings.flags.pclk_active_neg = true;
    config.data_width = 16;
    config.bits_per_pixel = 16;
    config.num_fbs = 2;
    config.bounce_buffer_size_px = board::LCD_WIDTH * 10;
    config.dma_burst_size = 64;
    config.sram_trans_align = 4;
    config.psram_trans_align = 64;
    config.hsync_gpio_num = GPIO_NUM_46;
    config.vsync_gpio_num = GPIO_NUM_3;
    config.de_gpio_num = GPIO_NUM_5;
    config.pclk_gpio_num = GPIO_NUM_7;
    config.disp_gpio_num = -1;
    const int data_pins[] = {
        GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17,
        GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_0, GPIO_NUM_45,
        GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_1,
        GPIO_NUM_2, GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
    };
    std::copy(std::begin(data_pins), std::end(data_pins), std::begin(config.data_gpio_nums));
    config.flags.fb_in_psram = true;

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, &panel), TAG, "RGB panel create failed");

    frame_mutex = xSemaphoreCreateMutex();
    frame_complete_semaphore = xSemaphoreCreateBinary();
    if ((frame_mutex == nullptr) || (frame_complete_semaphore == nullptr))
    {
        ESP_LOGE(TAG, "RGB frame synchronization allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "RGB panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "RGB panel init failed");

    void *first = nullptr;
    void *second = nullptr;
    const esp_err_t result = esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &first, &second);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "RGB framebuffer lookup failed: %s", esp_err_to_name(result));
        return result;
    }

    frame_buffers[0] = static_cast<std::uint16_t *>(first);
    frame_buffers[1] = static_cast<std::uint16_t *>(second);

    std::fill_n(frame_buffers[0], FRAME_PIXELS, 0x0000);
    std::fill_n(frame_buffers[1], FRAME_PIXELS, 0x0000);

    const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
        .on_bounce_frame_finish = on_frame_complete,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr), TAG,
                        "RGB frame callback registration failed");

    ESP_LOGI(TAG, "RGB LCD ready: %dx%d, 30 MHz, double RGB565 framebuffer with bounce DMA",
             board::LCD_WIDTH, board::LCD_HEIGHT);
    return ESP_OK;
}

bool is_ready()
{
    return (panel != nullptr) && (frame_buffers[0] != nullptr) && (frame_buffers[1] != nullptr) &&
           (frame_mutex != nullptr) && (frame_complete_semaphore != nullptr);
}

esp_err_t begin_frame()
{
    if (!is_ready())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(150)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    frame_in_progress = true;
    reset_view_transform();
    reset_clip_rect();
    return ESP_OK;
}

void set_view_transform(int offset_x, int offset_y, int scale_numerator, int scale_denominator)
{
    set_view_transform_xy(offset_x,
                          offset_y,
                          scale_numerator,
                          scale_denominator,
                          scale_numerator,
                          scale_denominator);
}

void set_view_transform_xy(int offset_x,
                           int offset_y,
                           int scale_x_numerator,
                           int scale_x_denominator,
                           int scale_y_numerator,
                           int scale_y_denominator)
{
    view_transform.offset_x = offset_x;
    view_transform.offset_y = offset_y;
    view_transform.scale_numerator = std::max(1, scale_x_numerator);
    view_transform.scale_denominator = std::max(1, scale_x_denominator);
    view_transform.scale_y_numerator = std::max(1, scale_y_numerator);
    view_transform.scale_y_denominator = std::max(1, scale_y_denominator);
}

void reset_view_transform()
{
    view_transform.offset_x = 0;
    view_transform.offset_y = 0;
    view_transform.scale_numerator = 1;
    view_transform.scale_denominator = 1;
    view_transform.scale_y_numerator = 1;
    view_transform.scale_y_denominator = 1;
}

void set_clip_rect(int x, int y, int width, int height)
{
    view_transform.clip_x = x;
    view_transform.clip_y = y;
    view_transform.clip_width = std::max(0, width);
    view_transform.clip_height = std::max(0, height);
}

void reset_clip_rect()
{
    view_transform.clip_x = 0;
    view_transform.clip_y = 0;
    view_transform.clip_width = board::LCD_WIDTH;
    view_transform.clip_height = board::LCD_HEIGHT;
}

esp_err_t present()
{
    if (!is_ready() || !frame_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_lcd_panel_draw_bitmap(panel,
                                                 0,
                                                 0,
                                                 board::LCD_WIDTH,
                                                 board::LCD_HEIGHT,
                                                 frame_buffers[drawing_frame_index]);
    if (result == ESP_OK)
    {
        // Wait for a complete bounce-buffer frame after requesting the buffer switch.
        clear_frame_complete_notification();
        if (xSemaphoreTake(frame_complete_semaphore, pdMS_TO_TICKS(250)) != pdTRUE)
        {
            ESP_LOGW(TAG, "RGB frame completion timed out");
            result = ESP_ERR_TIMEOUT;
        }
    }
    if (result == ESP_OK)
    {
        drawing_frame_index ^= 1U;
    }
    frame_in_progress = false;
    xSemaphoreGive(frame_mutex);
    return result;
}

esp_err_t fill(std::uint16_t color)
{
    if (!is_ready() || !frame_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }

    fill_rgb565(frame_buffers[drawing_frame_index], FRAME_PIXELS, color);
    return ESP_OK;
}

esp_err_t fill_rect(int x, int y, int width, int height, std::uint16_t color)
{
    const int transformed_x = transform_x(x);
    const int transformed_y = transform_y(y);
    const int transformed_right = transform_x(x + width);
    const int transformed_bottom = transform_y(y + height);
    return fill_rect_raw(transformed_x,
                         transformed_y,
                         transformed_right - transformed_x,
                         transformed_bottom - transformed_y,
                         color);
}

esp_err_t draw_rgb565(int x, int y, int width, int height, const std::uint16_t *pixels)
{
    if (!is_ready() || !frame_in_progress)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if ((pixels == nullptr) || (width <= 0) || (height <= 0))
    {
        return ESP_ERR_INVALID_ARG;
    }

    const int transformed_x = transform_x(x);
    const int transformed_y = transform_y(y);
    const int transformed_right = transform_x(x + width);
    const int transformed_bottom = transform_y(y + height);
    const int transformed_width = transformed_right - transformed_x;
    const int transformed_height = transformed_bottom - transformed_y;
    const int x_start = std::max({0, transformed_x, view_transform.clip_x});
    const int y_start = std::max({0, transformed_y, view_transform.clip_y});
    const int x_end = std::min({board::LCD_WIDTH, transformed_right, view_transform.clip_x + view_transform.clip_width});
    const int y_end = std::min({board::LCD_HEIGHT, transformed_bottom, view_transform.clip_y + view_transform.clip_height});
    if ((x_start >= x_end) || (y_start >= y_end))
    {
        return ESP_OK;
    }

    for (int row = y_start; row < y_end; ++row)
    {
        std::uint16_t *destination = frame_buffers[drawing_frame_index] + static_cast<std::size_t>(row) * board::LCD_WIDTH + x_start;
        const int source_y = std::clamp(((row - transformed_y) * height) / std::max(1, transformed_height), 0, height - 1);
        if ((transformed_width == width) && (x_start == transformed_x) && (x_end == transformed_right))
        {
            std::memcpy(destination,
                        pixels + static_cast<std::size_t>(source_y) * width,
                        static_cast<std::size_t>(width) * sizeof(std::uint16_t));
            continue;
        }
        std::uint32_t source_x_fixed = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(x_start - transformed_x) * width << 16U) /
            std::max(1, transformed_width));
        const std::uint32_t source_x_step = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(width) << 16U) / std::max(1, transformed_width));
        for (int column = x_start; column < x_end; ++column)
        {
            const int source_x = std::min<int>(width - 1, source_x_fixed >> 16U);
            *destination++ = pixels[static_cast<std::size_t>(source_y) * width + source_x];
            source_x_fixed += source_x_step;
        }
    }
    return ESP_OK;
}

void draw_line(int x0, int y0, int x1, int y1, std::uint16_t color, int thickness)
{
    if (!is_ready() || !frame_in_progress || (thickness <= 0))
    {
        return;
    }

    x0 = transform_x(x0);
    y0 = transform_y(y0);
    x1 = transform_x(x1);
    y1 = transform_y(y1);
    thickness = std::max(1, (thickness * view_transform.scale_numerator + view_transform.scale_denominator - 1) /
                              view_transform.scale_denominator);

    const int delta_x = std::abs(x1 - x0);
    const int delta_y = -std::abs(y1 - y0);
    const int step_x = x0 < x1 ? 1 : -1;
    const int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x + delta_y;

    while (true)
    {
        for (int offset_y = -(thickness / 2); offset_y <= (thickness / 2); ++offset_y)
        {
            for (int offset_x = -(thickness / 2); offset_x <= (thickness / 2); ++offset_x)
            {
                write_pixel(x0 + offset_x, y0 + offset_y, color);
            }
        }
        if ((x0 == x1) && (y0 == y1))
        {
            break;
        }
        const int doubled_error = 2 * error;
        if (doubled_error >= delta_y)
        {
            error += delta_y;
            x0 += step_x;
        }
        if (doubled_error <= delta_x)
        {
            error += delta_x;
            y0 += step_y;
        }
    }
}

void draw_text(int x, int y, int scale, const char *text, std::uint16_t color)
{
    if (!is_ready() || !frame_in_progress || (text == nullptr) || (scale <= 0))
    {
        return;
    }

    int cursor_x = x;
    for (const char *character = text; *character != '\0'; ++character)
    {
        char upper = *character;
        if ((upper >= 'a') && (upper <= 'z'))
        {
            upper = static_cast<char>(upper - ('a' - 'A'));
        }

        const std::uint8_t *glyph = glyph_for(upper);
        for (int column = 0; column < 5; ++column)
        {
            for (int row = 0; row < 7; ++row)
            {
                if ((glyph[column] & (1U << row)) == 0U)
                {
                    continue;
                }
                fill_rect(cursor_x + column * scale, y + row * scale, scale, scale, color);
            }
        }
        cursor_x += 6 * scale;
    }
}

void draw_text_sized(int x,
                     int y,
                     int character_width,
                     int character_height,
                     const char *text,
                     std::uint16_t color,
                     bool bold,
                     bool underlined,
                     bool crossed_out,
                     TextEncoding encoding)
{
    if (!is_ready() || !frame_in_progress || (text == nullptr) ||
        (character_width <= 0) || (character_height <= 0))
    {
        return;
    }

    int cursor_x = x;
    FontAssetSize font_size;
    const bool have_asset_font = select_font_size(character_width, character_height, font_size);
    for (const auto *character = reinterpret_cast<const unsigned char *>(text); *character != '\0'; ++character)
    {
        const std::uint16_t codepoint = decode_character(*character, encoding);
        if (have_asset_font &&
            (draw_asset_glyph(cursor_x, y, character_width, character_height, codepoint, color, bold, font_size) ||
             draw_asset_glyph(cursor_x, y, character_width, character_height, '?', color, bold, font_size)))
        {
            cursor_x += character_width;
            continue;
        }

        char upper = static_cast<char>((codepoint <= 0x7F) ? codepoint : '?');
        if ((upper >= 'a') && (upper <= 'z'))
        {
            upper = static_cast<char>(upper - ('a' - 'A'));
        }
        const std::uint8_t *glyph = glyph_for(upper);
        const int painted_width = std::max(1, character_width - std::max(1, character_width / 6));
        for (int column = 0; column < 5; ++column)
        {
            const int left = cursor_x + (column * painted_width) / 5;
            const int right = cursor_x + ((column + 1) * painted_width) / 5;
            for (int row = 0; row < 7; ++row)
            {
                if ((glyph[column] & (1U << row)) == 0U)
                {
                    continue;
                }
                const int top = y + (row * character_height) / 7;
                const int bottom = y + ((row + 1) * character_height) / 7;
                fill_rect(left, top, std::max(1, right - left), std::max(1, bottom - top), color);
                if (bold && (right < cursor_x + painted_width))
                {
                    fill_rect(right, top, 1, std::max(1, bottom - top), color);
                }
            }
        }
        cursor_x += character_width;
    }

    const int text_width = static_cast<int>(std::strlen(text)) * character_width;
    if (underlined)
    {
        fill_rect(x, y + character_height - 1, text_width, 1, color);
    }
    if (crossed_out)
    {
        fill_rect(x, y + character_height / 2, text_width, 1, color);
    }
}
}
