#include "vt_scene_renderer.hpp"

#include "board_pins.hpp"
#include "aux_n_manager.hpp"
#include "gnss_um982.hpp"
#include "lcd_panel.hpp"
#include "lvgl_terminal_view.hpp"
#include "object_pool_store.hpp"
#include "section_control.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#if ISOVT_ENABLE_AGISOSTACK
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server_managed_working_set.hpp"
#endif

namespace
{
constexpr char TAG[] = "vt_render";
constexpr std::uint16_t NULL_OBJECT_ID = 0xFFFF;
constexpr int SOFT_KEY_COLUMNS = 2;
constexpr int SOFT_KEY_ROWS = 6;
constexpr int CONTROL_PADDING = 2;
constexpr int LEFT_INFO_WIDTH = 200;
constexpr int VT_ORIGIN_X = 0;
constexpr std::uint16_t CHROME_BACKGROUND = 0x2945;
constexpr std::uint16_t CHROME_DARK = 0x1082;
constexpr std::uint16_t CHROME_ACTIVE = 0x05E0;
constexpr std::uint16_t CHROME_BORDER = 0x7BEF;
constexpr std::uint16_t CHROME_TEXT = 0xFFFF;

struct ScreenRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    vt_scene_renderer::HitTarget target;
};

std::mutex scene_mutex;
std::mutex hit_targets_mutex;
std::vector<ScreenRect> hit_targets;
bool scene_visible = false;
std::atomic_bool terminal_menu_open{ false };
vt_scene_renderer::TerminalSettings terminal_settings;
vt_scene_renderer::TerminalAction terminal_page = vt_scene_renderer::TerminalAction::ShowMachines;
std::string terminal_notice;
std::size_t aux_function_page = 0;
std::size_t aux_input_page = 0;

struct InputEditorState
{
    bool open = false;
    vt_scene_renderer::InputEditorKind kind = vt_scene_renderer::InputEditorKind::None;
    std::uint16_t object_id = NULL_OBJECT_ID;
    std::string value;
    std::size_t maximum_length = 32;
    std::vector<vt_scene_renderer::EditorListItem> list_items;
    std::uint16_t selected_index = 0;
    vt_scene_renderer::EditorTextEncoding text_encoding = vt_scene_renderer::EditorTextEncoding::ISO8859_1;
};

std::mutex input_editor_mutex;
InputEditorState input_editor;

struct VTLayout
{
    int data_mask_size;
    int working_set_bar_width;
    int working_set_slot_height;
    int working_set_gap;
    int soft_key_designator_size;
    int soft_key_width;
    int soft_key_height;
    int content_width;
    int scale_numerator;
    int scale_denominator;
    int screen_x;
    int data_mask_x;
    int soft_key_x;
};

VTLayout current_layout()
{
    const int data_mask_size = terminal_settings.data_mask_size == 600 ? 600 : 480;
    const int soft_key_designator_size = data_mask_size / 8;
    const int scale_numerator = data_mask_size == 480 ? 5 : 1;
    const int scale_denominator = data_mask_size == 480 ? 4 : 1;
    constexpr int working_set_bar_screen_width = 136;
    const int working_set_bar_width = working_set_bar_screen_width * scale_denominator / scale_numerator;
    const int working_set_slot_height = working_set_bar_width;
    const int working_set_gap = data_mask_size / 120;
    const int data_mask_x = VT_ORIGIN_X + working_set_bar_width + working_set_gap;
    const int soft_key_x = data_mask_x + data_mask_size;
    const int soft_key_height = data_mask_size / SOFT_KEY_ROWS;
    const int soft_key_width = soft_key_height;
    const int content_width = soft_key_x + SOFT_KEY_COLUMNS * soft_key_width;
    const int scaled_width = content_width * scale_numerator / scale_denominator;
    const int screen_x = std::max(0, board::LCD_WIDTH - scaled_width);
    return {
        .data_mask_size = data_mask_size,
        .working_set_bar_width = working_set_bar_width,
        .working_set_slot_height = working_set_slot_height,
        .working_set_gap = working_set_gap,
        .soft_key_designator_size = soft_key_designator_size,
        .soft_key_width = soft_key_width,
        .soft_key_height = soft_key_height,
        .content_width = content_width,
        .scale_numerator = scale_numerator,
        .scale_denominator = scale_denominator,
        .screen_x = screen_x,
        .data_mask_x = data_mask_x,
        .soft_key_x = soft_key_x,
    };
}

int screen_x(int logical_x)
{
    const auto layout = current_layout();
    return layout.screen_x + logical_x * layout.scale_numerator / layout.scale_denominator;
}

int screen_y(int logical_y)
{
    const auto layout = current_layout();
    return logical_y * layout.scale_numerator / layout.scale_denominator;
}

int screen_width(int logical_x, int logical_width)
{
    return screen_x(logical_x + logical_width) - screen_x(logical_x);
}

int screen_height(int logical_y, int logical_height)
{
    return screen_y(logical_y + logical_height) - screen_y(logical_y);
}

void set_logical_clip(int x, int y, int width, int height)
{
    lcd_panel::set_clip_rect(screen_x(x), screen_y(y), screen_width(x, width), screen_height(y, height));
}

#if ISOVT_ENABLE_AGISOSTACK
using ObjectTree = std::map<std::uint16_t, std::shared_ptr<isobus::VTObject>>;

std::uint64_t working_set_name(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    const auto control_function = working_set ? working_set->get_control_function() : nullptr;
    return control_function ? control_function->get_NAME().get_full_name() : 0;
}

struct RenderContext
{
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set;
    const ObjectTree &object_tree;
    std::uint16_t data_mask_id;
    std::vector<ScreenRect> &targets;
};

std::uint16_t to_rgb565(const isobus::VTColourVector &colour)
{
    const auto clamp = [](float value, int maximum) {
        if (value <= 0.0F)
        {
            return 0;
        }
        if (value >= 1.0F)
        {
            return maximum;
        }
        return static_cast<int>(value * maximum + 0.5F);
    };

    const auto red = static_cast<std::uint16_t>(clamp(colour.r, 31));
    const auto green = static_cast<std::uint16_t>(clamp(colour.g, 63));
    const auto blue = static_cast<std::uint16_t>(clamp(colour.b, 31));
    return static_cast<std::uint16_t>((red << 11U) | (green << 5U) | blue);
}

std::uint16_t colour(RenderContext &context, std::uint8_t index)
{
    return to_rgb565(context.working_set->get_colour(index));
}

std::uint16_t contrasting_text_colour(std::uint16_t background)
{
    const std::uint16_t red = (background >> 11U) & 0x1FU;
    const std::uint16_t green = (background >> 5U) & 0x3FU;
    const std::uint16_t blue = background & 0x1FU;
    const std::uint16_t brightness = static_cast<std::uint16_t>(red * 2U + green + blue * 2U);
    return brightness > 110U ? 0x0000 : 0xFFFF;
}

std::shared_ptr<isobus::VTObject> find_object(const ObjectTree &tree, std::uint16_t object_id)
{
    const auto iterator = tree.find(object_id);
    return iterator == tree.end() ? nullptr : iterator->second;
}

std::shared_ptr<isobus::VTObject> resolve_object_pointer_chain(
    const ObjectTree &tree,
    std::shared_ptr<isobus::VTObject> object)
{
    constexpr std::uint8_t MAX_POINTER_DEPTH = 16;
    for (std::uint8_t depth = 0;
         object && (object->get_object_type() == isobus::VirtualTerminalObjectType::ObjectPointer) &&
         (depth < MAX_POINTER_DEPTH);
         ++depth)
    {
        object = find_object(tree, std::static_pointer_cast<isobus::ObjectPointer>(object)->get_value());
    }
    return object;
}

std::uint16_t colour_attribute(RenderContext &context,
                               const std::shared_ptr<isobus::VTObject> &object,
                               std::uint8_t attribute_id,
                               std::uint16_t fallback)
{
    std::uint32_t raw_value = 0;
    if ((object != nullptr) && object->get_attribute(attribute_id, raw_value))
    {
        return colour(context, static_cast<std::uint8_t>(raw_value));
    }
    return fallback;
}

int numeric_attribute(const std::shared_ptr<isobus::VTObject> &object, std::uint8_t attribute_id, int fallback)
{
    std::uint32_t raw_value = 0;
    if (object && object->get_attribute(attribute_id, raw_value))
    {
        return std::clamp(static_cast<int>(raw_value), 1, 8);
    }
    return fallback;
}

std::uint16_t line_colour(RenderContext &context, std::uint16_t line_attributes_id, std::uint16_t fallback)
{
    const auto attributes = find_object(context.object_tree, line_attributes_id);
    return colour_attribute(context,
                            attributes,
                            static_cast<std::uint8_t>(isobus::LineAttributes::AttributeName::LineColour),
                            fallback);
}

int line_width(RenderContext &context, std::uint16_t line_attributes_id)
{
    const auto attributes = find_object(context.object_tree, line_attributes_id);
    return numeric_attribute(attributes,
                             static_cast<std::uint8_t>(isobus::LineAttributes::AttributeName::LineWidth),
                             1);
}

std::uint16_t line_art_pattern(RenderContext &context, std::uint16_t line_attributes_id)
{
    const auto attributes = find_object(context.object_tree, line_attributes_id);
    if ((attributes == nullptr) ||
        (attributes->get_object_type() != isobus::VirtualTerminalObjectType::LineAttributes))
    {
        return 0xFFFF;
    }
    return std::static_pointer_cast<isobus::LineAttributes>(attributes)->get_line_art_bit_pattern();
}

bool fill_colour(RenderContext &context,
                 std::uint16_t fill_attributes_id,
                 std::uint16_t line_colour_value,
                 std::uint16_t fallback,
                 std::uint16_t &result)
{
    const auto attributes = find_object(context.object_tree, fill_attributes_id);
    if ((attributes == nullptr) || (attributes->get_object_type() != isobus::VirtualTerminalObjectType::FillAttributes))
    {
        result = fallback;
        return true;
    }

    const auto fill = std::static_pointer_cast<isobus::FillAttributes>(attributes);
    switch (fill->get_type())
    {
        case isobus::FillAttributes::FillType::NoFill:
            return false;
        case isobus::FillAttributes::FillType::FillWithLineColor:
            result = line_colour_value;
            return true;
        case isobus::FillAttributes::FillType::FillWithSpecifiedColorInFillColorAttribute:
        case isobus::FillAttributes::FillType::FillWithPatternGivenByFillPatternAttribute:
            result = colour_attribute(context,
                                      fill,
                                      static_cast<std::uint8_t>(isobus::FillAttributes::AttributeName::FillColour),
                                      fallback);
            return true;
    }
    return false;
}

struct FontRenderInfo
{
    int width = 6;
    int height = 8;
    std::uint16_t colour = 0x0000;
    bool bold = false;
    bool underlined = false;
    bool crossed_out = false;
    lcd_panel::TextEncoding encoding = lcd_panel::TextEncoding::ISO8859_1;
};

FontRenderInfo font_info(RenderContext &context,
                         const std::shared_ptr<isobus::VTObject> &object,
                         std::uint16_t font_attributes_id,
                         std::uint16_t fallback_colour)
{
    FontRenderInfo result;
    result.height = std::max(1, std::min<int>(object->get_height(), 8));
    result.width = std::max(1, result.height * 6 / 8);
    result.colour = fallback_colour;

    const auto attributes = find_object(context.object_tree, font_attributes_id);
    if ((attributes == nullptr) || (attributes->get_object_type() != isobus::VirtualTerminalObjectType::FontAttributes))
    {
        return result;
    }

    const auto font = std::static_pointer_cast<isobus::FontAttributes>(attributes);
    const int requested_width = std::max(1, static_cast<int>(font->get_font_width_pixels()));
    const int requested_height = std::max(1, static_cast<int>(font->get_font_height_pixels()));
    result.height = std::min<int>(object->get_height(), requested_height);
    result.width = std::max(1, requested_width * result.height / requested_height);
    result.colour = colour(context, font->get_colour());
    result.bold = font->get_style(isobus::FontAttributes::FontStyleBits::Bold);
    result.underlined = font->get_style(isobus::FontAttributes::FontStyleBits::Underlined);
    result.crossed_out = font->get_style(isobus::FontAttributes::FontStyleBits::CrossedOut);
    switch (font->get_type())
    {
        case isobus::FontAttributes::FontType::ISO8859_15:
            result.encoding = lcd_panel::TextEncoding::ISO8859_15;
            break;
        case isobus::FontAttributes::FontType::ISO8859_2:
            result.encoding = lcd_panel::TextEncoding::ISO8859_2;
            break;
        case isobus::FontAttributes::FontType::ISO8859_4:
            result.encoding = lcd_panel::TextEncoding::ISO8859_4;
            break;
        case isobus::FontAttributes::FontType::ISO8859_5:
            result.encoding = lcd_panel::TextEncoding::ISO8859_5;
            break;
        case isobus::FontAttributes::FontType::ISO8859_7:
            result.encoding = lcd_panel::TextEncoding::ISO8859_7;
            break;
        default:
            result.encoding = lcd_panel::TextEncoding::ISO8859_1;
            break;
    }
    return result;
}

void draw_outline(int x, int y, int width, int height, std::uint16_t colour_value,
                  int thickness = 2, std::uint8_t suppressed_edges = 0)
{
    if ((width <= 0) || (height <= 0) || (thickness <= 0))
    {
        return;
    }
    const int edge = std::min({ thickness, width, height });
    // ISO OutputRectangle LineSuppression: top, right, bottom, left.
    if ((suppressed_edges & 0x01U) == 0U)
        lcd_panel::fill_rect(x, y, width, edge, colour_value);
    if ((suppressed_edges & 0x04U) == 0U)
        lcd_panel::fill_rect(x, y + height - edge, width, edge, colour_value);
    if ((suppressed_edges & 0x08U) == 0U)
        lcd_panel::fill_rect(x, y, edge, height, colour_value);
    if ((suppressed_edges & 0x02U) == 0U)
        lcd_panel::fill_rect(x + width - edge, y, edge, height, colour_value);
}

std::string clipped_text(const std::string &text, int width, int character_width)
{
    if ((width <= 0) || (character_width <= 0))
    {
        return {};
    }
    const std::size_t capacity = static_cast<std::size_t>(width / character_width);
    return text.substr(0, capacity);
}

std::vector<std::string> split_text_lines(const std::string &text)
{
    std::vector<std::string> lines(1);
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        const char character = text[index];
        if ((character == '\r') || (character == '\n'))
        {
            if ((character == '\r') && ((index + 1) < text.size()) && (text[index + 1] == '\n'))
            {
                ++index;
            }
            lines.emplace_back();
        }
        else
        {
            lines.back().push_back(character);
        }
    }
    return lines;
}

std::vector<std::string> wrap_text_lines(const std::string &text,
                                         std::size_t capacity,
                                         bool auto_wrap)
{
    auto source_lines = split_text_lines(text);
    if (!auto_wrap || (capacity == 0))
    {
        return source_lines;
    }

    std::vector<std::string> result;
    for (const auto &source : source_lines)
    {
        if (source.empty())
        {
            result.emplace_back();
            continue;
        }

        std::size_t offset = 0;
        while (offset < source.size())
        {
            while ((offset < source.size()) && (source[offset] == ' '))
            {
                ++offset;
            }
            if (offset >= source.size())
            {
                break;
            }
            if ((source.size() - offset) <= capacity)
            {
                result.push_back(source.substr(offset));
                break;
            }

            std::size_t split = source.rfind(' ', offset + capacity);
            const std::size_t hyphen = source.rfind('-', offset + capacity - 1);
            if ((hyphen != std::string::npos) && (hyphen >= offset) &&
                ((split == std::string::npos) || (split < offset) || (hyphen > split)))
            {
                split = hyphen + 1;
            }
            if ((split == std::string::npos) || (split <= offset))
            {
                split = offset + capacity;
            }

            std::string line = source.substr(offset, split - offset);
            while (!line.empty() && (line.back() == ' '))
            {
                line.pop_back();
            }
            result.push_back(std::move(line));
            offset = split;
        }
    }
    return result.empty() ? std::vector<std::string>{ "" } : result;
}

std::uint8_t unicode_to_iso8859_2(std::uint16_t value)
{
    if (value < 0x80U)
    {
        return static_cast<std::uint8_t>(value);
    }
    switch (value)
    {
        case 0x0104: return 0xA1; // A ogonek
        case 0x0141: return 0xA3; // L stroke
        case 0x015A: return 0xA6; // S acute
        case 0x0179: return 0xAC; // Z acute
        case 0x017B: return 0xAF; // Z dot
        case 0x0105: return 0xB1;
        case 0x0142: return 0xB3;
        case 0x015B: return 0xB6;
        case 0x017A: return 0xBC;
        case 0x017C: return 0xBF;
        case 0x0106: return 0xC6;
        case 0x0118: return 0xCA;
        case 0x0143: return 0xD1;
        case 0x00D3: return 0xD3;
        case 0x0107: return 0xE6;
        case 0x0119: return 0xEA;
        case 0x0144: return 0xF1;
        case 0x00F3: return 0xF3;
        default: return static_cast<std::uint8_t>('?');
    }
}

std::string display_text(const std::string &value, lcd_panel::TextEncoding &encoding)
{
    if ((value.size() < 2) ||
        (static_cast<std::uint8_t>(value[0]) != 0xFFU) ||
        (static_cast<std::uint8_t>(value[1]) != 0xFEU))
    {
        // VT strings have a fixed declared length and may contain padding after
        // their terminator. Do not render those bytes as garbage characters.
        const auto terminator = value.find('\0');
        return value.substr(0, terminator);
    }

    std::string decoded;
    decoded.reserve((value.size() - 2) / 2);
    for (std::size_t index = 2; (index + 1) < value.size(); index += 2)
    {
        const auto codepoint = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(value[index]) |
            (static_cast<std::uint16_t>(static_cast<std::uint8_t>(value[index + 1])) << 8));
        if (codepoint == 0U)
        {
            break;
        }
        decoded.push_back(static_cast<char>(unicode_to_iso8859_2(codepoint)));
    }
    encoding = lcd_panel::TextEncoding::ISO8859_2;
    return decoded;
}

void draw_textual_object(RenderContext &context,
                         const std::shared_ptr<isobus::VTObject> &object,
                         int x,
                         int y,
                         const std::string &text,
                         std::uint16_t font_attributes_id = NULL_OBJECT_ID,
                         bool transparent = false)
{
    const int width = object->get_width();
    const int height = object->get_height();
    const std::uint16_t background = colour(context, object->get_background_color());
    if (!transparent)
    {
        lcd_panel::fill_rect(x, y, width, height, background);
    }

    auto font = font_info(context, object, font_attributes_id, contrasting_text_colour(background));
    const std::string shown_text = display_text(text, font.encoding);
    bool auto_wrap = false;
    if ((object->get_object_type() == isobus::VirtualTerminalObjectType::InputString) ||
        (object->get_object_type() == isobus::VirtualTerminalObjectType::OutputString))
    {
        auto_wrap = std::static_pointer_cast<isobus::StringVTObject>(object)->get_option(
            isobus::StringVTObject::Options::AutoWrap);
    }
    const std::size_t line_capacity = static_cast<std::size_t>(
        std::max(1, width / std::max(1, font.width)));
    auto lines = wrap_text_lines(shown_text, line_capacity, auto_wrap);
    const std::size_t maximum_lines = static_cast<std::size_t>(std::max(1, height / font.height));
    if (lines.size() > maximum_lines)
    {
        lines.resize(maximum_lines);
    }
    for (auto &line : lines)
    {
        line = clipped_text(line, width, font.width);
    }

    const auto textual = std::static_pointer_cast<isobus::TextualVTObject>(object);
    const int text_block_height = static_cast<int>(lines.size()) * font.height;
    int text_y = y;
    if (textual->get_vertical_justification() == isobus::TextualVTObject::VerticalJustification::PositionMiddle)
    {
        text_y += std::max(0, (height - text_block_height) / 2);
    }
    else if (textual->get_vertical_justification() == isobus::TextualVTObject::VerticalJustification::PositionBottom)
    {
        text_y += std::max(0, height - text_block_height);
    }

    for (const auto &line : lines)
    {
        const int text_width = static_cast<int>(line.size()) * font.width;
        int text_x = x;
        if (textual->get_horizontal_justification() == isobus::TextualVTObject::HorizontalJustification::PositionMiddle)
        {
            text_x += std::max(0, (width - text_width) / 2);
        }
        else if (textual->get_horizontal_justification() == isobus::TextualVTObject::HorizontalJustification::PositionRight)
        {
            text_x += std::max(0, width - text_width);
        }

        lcd_panel::draw_text_sized(text_x,
                                   text_y,
                                   font.width,
                                   font.height,
                                   line.c_str(),
                                   font.colour,
                                   font.bold,
                                   font.underlined,
                                   font.crossed_out,
                                   font.encoding);
        text_y += font.height;
    }
}

std::string format_number(RenderContext &context,
                          const std::shared_ptr<isobus::NumberVTObject> &number)
{
    char buffer[40];
    const int decimals = std::min<int>(number->get_number_of_decimals(), 4);
    std::uint32_t raw_value = number->get_value();
    const auto variable = find_object(context.object_tree, number->get_variable_reference());
    if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
    {
        raw_value = std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value();
    }
    const double displayed = (static_cast<double>(raw_value) + number->get_offset()) * number->get_scale();
    if (number->get_option(isobus::NumberVTObject::Options::DisplayZeroAsBlank) && (displayed == 0.0))
    {
        return {};
    }
    if (number->get_format())
    {
        std::snprintf(buffer, sizeof(buffer), "%.*E", decimals, displayed);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, displayed);
    }
    return buffer;
}

void add_target(RenderContext &context,
                int x,
                int y,
                int width,
                int height,
                vt_scene_renderer::HitKind kind,
                std::uint16_t object_id,
                std::uint8_t key_code = 0,
                std::uint16_t parent_id = NULL_OBJECT_ID);
void render_object(RenderContext &context,
                   const std::shared_ptr<isobus::VTObject> &object,
                   int x,
                   int y,
                   int depth);

std::uint16_t selected_list_index(RenderContext &context,
                                  const std::shared_ptr<isobus::ListVTObject> &list)
{
    const auto variable = find_object(context.object_tree, list->get_variable_reference());
    if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
    {
        return static_cast<std::uint16_t>(std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value());
    }
    return list->get_value();
}

void render_list_object(RenderContext &context,
                        const std::shared_ptr<isobus::ListVTObject> &list,
                        int x,
                        int y,
                        int depth,
                        bool interactive)
{
    const std::uint16_t selected = selected_list_index(context, list);
    if (selected < list->get_number_children())
    {
        render_object(context,
                      find_object(context.object_tree, list->get_child_id(selected)),
                      x,
                      y,
                      depth + 1);
    }
    if (interactive)
    {
        add_target(context,
                   x,
                   y,
                   list->get_width(),
                   list->get_height(),
                   vt_scene_renderer::HitKind::InputList,
                   list->get_id());
    }
}

void render_children(RenderContext &context,
                     const std::shared_ptr<isobus::VTObject> &parent,
                     int parent_x,
                     int parent_y,
                     int depth);

bool first_child_covers_data_mask(RenderContext &context,
                                  const std::shared_ptr<isobus::VTObject> &data_mask,
                                  int data_mask_size)
{
    if (!data_mask || (data_mask->get_number_children() == 0))
    {
        return false;
    }
    const auto first_child = find_object(context.object_tree, data_mask->get_child_id(0));
    if (!first_child ||
        (first_child->get_object_type() != isobus::VirtualTerminalObjectType::OutputRectangle) ||
        (data_mask->get_child_x(0) > 0) ||
        (data_mask->get_child_y(0) > 0) ||
        (first_child->get_width() < data_mask_size) ||
        (first_child->get_height() < data_mask_size))
    {
        return false;
    }
    const auto rectangle = std::static_pointer_cast<isobus::OutputRectangle>(first_child);
    std::uint16_t ignored_colour = 0;
    return fill_colour(context,
                       rectangle->get_fill_attributes(),
                       line_colour(context, rectangle->get_line_attributes(), 0),
                       0,
                       ignored_colour);
}

void add_target(RenderContext &context,
                int x,
                int y,
                int width,
                int height,
                vt_scene_renderer::HitKind kind,
                std::uint16_t object_id,
                std::uint8_t key_code,
                std::uint16_t parent_id)
{
    if ((width <= 0) || (height <= 0))
    {
        return;
    }
    context.targets.push_back({
        .x = screen_x(x),
        .y = screen_y(y),
        .width = screen_width(x, width),
        .height = screen_height(y, height),
        .target = {
            .kind = kind,
            .object_id = object_id,
            .parent_id = parent_id == NULL_OBJECT_ID ? context.data_mask_id : parent_id,
            .key_code = key_code,
        },
    });
}

void draw_picture(RenderContext &context,
                  const std::shared_ptr<isobus::PictureGraphic> &picture,
                  int x,
                  int y)
{
    const int source_width = picture->get_actual_width();
    const int source_height = picture->get_actual_height();
    const int target_width = picture->get_width();
    const int target_height = picture->get_height();
    const auto &pixels = picture->get_raw_data();
    if ((source_width <= 0) || (source_height <= 0) || (target_width <= 0) || (target_height <= 0) ||
        (pixels.size() < static_cast<std::size_t>(source_width * source_height)))
    {
        return;
    }

    const bool transparent = picture->get_option(isobus::PictureGraphic::Options::Transparent);
    const std::uint8_t transparent_colour = picture->get_transparency_colour();
    std::vector<std::uint16_t> row(static_cast<std::size_t>(target_width));

    for (int target_y = 0; target_y < target_height; ++target_y)
    {
        const int source_y = (target_y * source_height) / target_height;
        for (int target_x = 0; target_x < target_width; ++target_x)
        {
            const int source_x = (target_x * source_width) / target_width;
            row[static_cast<std::size_t>(target_x)] =
                colour(context, pixels[static_cast<std::size_t>(source_y * source_width + source_x)]);
        }

        if (!transparent)
        {
            lcd_panel::draw_rgb565(x, y + target_y, target_width, 1, row.data());
            continue;
        }

        int run_start = -1;
        for (int target_x = 0; target_x <= target_width; ++target_x)
        {
            const bool opaque = (target_x < target_width) &&
                                (pixels[static_cast<std::size_t>(
                                     source_y * source_width + ((target_x * source_width) / target_width))] != transparent_colour);
            if (opaque && (run_start < 0))
            {
                run_start = target_x;
            }
            else if (!opaque && (run_start >= 0))
            {
                lcd_panel::draw_rgb565(x + run_start,
                                       y + target_y,
                                       target_x - run_start,
                                       1,
                                       row.data() + run_start);
                run_start = -1;
            }
        }
    }
}

void draw_polygon(RenderContext &context,
                  const std::shared_ptr<isobus::OutputPolygon> &polygon,
                  int x,
                  int y)
{
    const auto point_count = polygon->get_number_of_points();
    if (point_count < 2)
    {
        return;
    }

    const std::uint16_t edge_colour = line_colour(context, polygon->get_line_attributes(), 0xFFFF);
    const int edge_width = line_width(context, polygon->get_line_attributes());
    if (polygon->get_type() != isobus::OutputPolygon::PolygonType::Open)
    {
        std::uint16_t body_colour = edge_colour;
        if (fill_colour(context, polygon->get_fill_attributes(), edge_colour, edge_colour, body_colour))
        {
            for (int scan_y = 0; scan_y < polygon->get_height(); ++scan_y)
            {
                std::vector<int> intersections;
                intersections.reserve(point_count);
                for (std::uint8_t index = 0; index < point_count; ++index)
                {
                    const auto first = polygon->get_point(index);
                    const auto second = polygon->get_point(static_cast<std::uint8_t>((index + 1) % point_count));
                    if (((first.yValue <= scan_y) && (second.yValue > scan_y)) ||
                        ((second.yValue <= scan_y) && (first.yValue > scan_y)))
                    {
                        const int intersection = first.xValue +
                                                 ((scan_y - first.yValue) *
                                                  (static_cast<int>(second.xValue) - static_cast<int>(first.xValue))) /
                                                     (static_cast<int>(second.yValue) - static_cast<int>(first.yValue));
                        intersections.push_back(intersection);
                    }
                }
                std::sort(intersections.begin(), intersections.end());
                for (std::size_t index = 0; index + 1 < intersections.size(); index += 2)
                {
                    lcd_panel::fill_rect(x + intersections[index],
                                         y + scan_y,
                                         std::max(1, intersections[index + 1] - intersections[index] + 1),
                                         1,
                                         body_colour);
                }
            }
        }
    }

    for (std::uint8_t index = 0; index + 1 < point_count; ++index)
    {
        const auto first = polygon->get_point(index);
        const auto second = polygon->get_point(static_cast<std::uint8_t>(index + 1));
        lcd_panel::draw_line(x + first.xValue, y + first.yValue, x + second.xValue, y + second.yValue, edge_colour, edge_width);
    }
    if (polygon->get_type() != isobus::OutputPolygon::PolygonType::Open)
    {
        const auto first = polygon->get_point(0);
        const auto last = polygon->get_point(static_cast<std::uint8_t>(point_count - 1));
        lcd_panel::draw_line(x + last.xValue, y + last.yValue, x + first.xValue, y + first.yValue, edge_colour, edge_width);
    }
}

void draw_ellipse(RenderContext &context,
                  const std::shared_ptr<isobus::OutputEllipse> &ellipse,
                  int x,
                  int y)
{
    const int width = ellipse->get_width();
    const int height = ellipse->get_height();
    if ((width <= 1) || (height <= 1))
    {
        return;
    }

    const double radius_x = width / 2.0;
    const double radius_y = height / 2.0;
    const double centre_x = x + radius_x;
    const double centre_y = y + radius_y;
    const std::uint16_t background = colour(context, ellipse->get_background_color());
    const std::uint16_t edge_colour = line_colour(context, ellipse->get_line_attributes(), contrasting_text_colour(background));
    std::uint16_t body_colour = background;
    const bool has_fill = fill_colour(context, ellipse->get_fill_attributes(), edge_colour, background, body_colour);
    const int edge_width = line_width(context, ellipse->get_line_attributes());

    if (has_fill)
    {
        for (int row = 0; row < height; ++row)
        {
            const double normalized_y = (row - radius_y) / radius_y;
            const int half_span = static_cast<int>(radius_x * std::sqrt(std::max(0.0, 1.0 - normalized_y * normalized_y)));
            lcd_panel::fill_rect(static_cast<int>(centre_x) - half_span, y + row, half_span * 2 + 1, 1, body_colour);
        }
    }
    for (int degrees = 0; degrees < 360; ++degrees)
    {
        constexpr double PI = 3.14159265358979323846;
        const double angle = degrees * PI / 180.0;
        lcd_panel::fill_rect(static_cast<int>(centre_x + radius_x * std::cos(angle)),
                             static_cast<int>(centre_y - radius_y * std::sin(angle)),
                             edge_width,
                             edge_width,
                             edge_colour);
    }
}

std::uint16_t referenced_u16_value(RenderContext &context, std::uint16_t reference, std::uint16_t fallback)
{
    const auto variable = find_object(context.object_tree, reference);
    if (variable && (variable->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable))
    {
        return static_cast<std::uint16_t>(std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value());
    }
    return fallback;
}

double normalized_value(std::uint16_t value, std::uint16_t minimum, std::uint16_t maximum)
{
    if (maximum <= minimum)
    {
        return 0.0;
    }
    return std::clamp((static_cast<double>(value) - minimum) / (maximum - minimum), 0.0, 1.0);
}

void draw_linear_bar_graph(RenderContext &context,
                           const std::shared_ptr<isobus::OutputLinearBarGraph> &bar,
                           int x,
                           int y)
{
    const int width = bar->get_width();
    const int height = bar->get_height();
    const std::uint16_t bar_colour = colour(context, bar->get_colour());
    const std::uint16_t value = referenced_u16_value(context, bar->get_variable_reference(), bar->get_value());
    const double ratio = normalized_value(value, bar->get_min_value(), bar->get_max_value());
    const bool horizontal = bar->get_option(isobus::OutputLinearBarGraph::Options::AxisOrientation);
    const bool positive = bar->get_option(isobus::OutputLinearBarGraph::Options::Direction);
    const bool outline_only = bar->get_option(isobus::OutputLinearBarGraph::Options::BarGraphType);
    if (bar->get_option(isobus::OutputLinearBarGraph::Options::DrawBorder))
    {
        draw_outline(x, y, width, height, bar_colour);
    }
    if (horizontal)
    {
        const int amount = std::clamp(static_cast<int>(std::lround(width * ratio)), 0, width);
        const int value_x = positive ? x : x + width - amount;
        if (outline_only)
        {
            lcd_panel::draw_line(value_x + (positive ? amount : 0), y, value_x + (positive ? amount : 0), y + height - 1, bar_colour, 2);
        }
        else if (amount > 0)
        {
            lcd_panel::fill_rect(value_x, y, amount, height, bar_colour);
        }
    }
    else
    {
        const int amount = std::clamp(static_cast<int>(std::lround(height * ratio)), 0, height);
        const int value_y = positive ? y + height - amount : y;
        if (outline_only)
        {
            lcd_panel::draw_line(x, value_y, x + width - 1, value_y, bar_colour, 2);
        }
        else if (amount > 0)
        {
            lcd_panel::fill_rect(x, value_y, width, amount, bar_colour);
        }
    }
    if (bar->get_option(isobus::OutputLinearBarGraph::Options::DrawTargetLine))
    {
        const std::uint16_t target = referenced_u16_value(context, bar->get_target_value_reference(), bar->get_target_value());
        const double target_ratio = normalized_value(target, bar->get_min_value(), bar->get_max_value());
        const std::uint16_t target_colour = colour(context, bar->get_target_line_colour());
        if (horizontal)
        {
            const int target_x = x + static_cast<int>(std::lround((positive ? target_ratio : 1.0 - target_ratio) * width));
            lcd_panel::draw_line(target_x, y, target_x, y + height - 1, target_colour, 2);
        }
        else
        {
            const int target_y = y + static_cast<int>(std::lround((positive ? 1.0 - target_ratio : target_ratio) * height));
            lcd_panel::draw_line(x, target_y, x + width - 1, target_y, target_colour, 2);
        }
    }
}

void draw_meter(RenderContext &context,
                const std::shared_ptr<isobus::OutputMeter> &meter,
                int x,
                int y)
{
    constexpr double PI = 3.14159265358979323846;
    const int width = meter->get_width();
    const int radius = std::max(1, width / 2 - 2);
    const int cx = x + width / 2;
    const int cy = y + width / 2;
    const double start = meter->get_start_angle() * 2.0;
    double sweep = meter->get_end_angle() * 2.0 - start;
    if (sweep <= 0.0)
    {
        sweep += 360.0;
    }
    const bool clockwise = meter->get_option(isobus::OutputMeter::Options::DeflectionDirection);
    const std::uint16_t value = referenced_u16_value(context, meter->get_variable_reference(), meter->get_value());
    const double ratio = normalized_value(value, meter->get_min_value(), meter->get_max_value());
    const std::uint16_t arc_colour = colour(context, meter->get_arc_and_tick_colour());
    if (meter->get_option(isobus::OutputMeter::Options::DrawArc))
    {
        for (int step = 0; step <= static_cast<int>(sweep); ++step)
        {
            const double degrees = start + (clockwise ? -step : step);
            const double radians = degrees * PI / 180.0;
            lcd_panel::fill_rect(cx + static_cast<int>(radius * std::cos(radians)),
                                 cy - static_cast<int>(radius * std::sin(radians)), 2, 2, arc_colour);
        }
    }
    if (meter->get_option(isobus::OutputMeter::Options::DrawBorder))
    {
        draw_outline(x, y, width, width, colour(context, meter->get_border_colour()));
    }
    const double needle_degrees = start + (clockwise ? -1.0 : 1.0) * sweep * ratio;
    const double needle_radians = needle_degrees * PI / 180.0;
    lcd_panel::draw_line(cx, cy,
                         cx + static_cast<int>((radius - 4) * std::cos(needle_radians)),
                         cy - static_cast<int>((radius - 4) * std::sin(needle_radians)),
                         colour(context, meter->get_needle_colour()), 2);
}

void draw_arched_bar_graph(RenderContext &context,
                           const std::shared_ptr<isobus::OutputArchedBarGraph> &bar,
                           int x,
                           int y)
{
    constexpr double PI = 3.14159265358979323846;
    const int width = bar->get_width();
    const int height = bar->get_height();
    const int cx = x + width / 2;
    const int cy = y + height / 2;
    const double radius_x = std::max(1.0, width / 2.0 - 2.0);
    const double radius_y = std::max(1.0, height / 2.0 - 2.0);
    const double start = bar->get_start_angle() * 2.0;
    double sweep = bar->get_end_angle() * 2.0 - start;
    if (sweep <= 0.0)
    {
        sweep += 360.0;
    }
    const bool clockwise = bar->get_option(isobus::OutputArchedBarGraph::Options::Deflection);
    const std::uint16_t value = referenced_u16_value(context, bar->get_variable_reference(), bar->get_value());
    const int painted_degrees = static_cast<int>(std::lround(sweep * normalized_value(value, bar->get_min_value(), bar->get_max_value())));
    const int thickness = std::max(1, static_cast<int>(bar->get_bar_graph_width()));
    const std::uint16_t graph_colour = colour(context, bar->get_colour());
    for (int step = 0; step <= painted_degrees; ++step)
    {
        const double degrees = start + (clockwise ? -step : step);
        const double radians = degrees * PI / 180.0;
        lcd_panel::fill_rect(cx + static_cast<int>(radius_x * std::cos(radians)) - thickness / 2,
                             cy - static_cast<int>(radius_y * std::sin(radians)) - thickness / 2,
                             thickness, thickness, graph_colour);
    }
    if (bar->get_option(isobus::OutputArchedBarGraph::Options::DrawBorder))
    {
        draw_outline(x, y, width, height, graph_colour);
    }
    if (bar->get_option(isobus::OutputArchedBarGraph::Options::DrawTargetLine))
    {
        const std::uint16_t target = referenced_u16_value(context, bar->get_target_value_reference(), bar->get_target_value());
        const double target_degrees = start + (clockwise ? -1.0 : 1.0) * sweep *
                                                  normalized_value(target, bar->get_min_value(), bar->get_max_value());
        const double radians = target_degrees * PI / 180.0;
        lcd_panel::fill_rect(cx + static_cast<int>(radius_x * std::cos(radians)) - thickness,
                             cy - static_cast<int>(radius_y * std::sin(radians)) - thickness,
                             thickness * 2, thickness * 2, colour(context, bar->get_target_line_colour()));
    }
}

void render_object(RenderContext &context,
                   const std::shared_ptr<isobus::VTObject> &object,
                   int x,
                   int y,
                   int depth)
{
    if ((object == nullptr) || (depth > 20))
    {
        return;
    }

    using Type = isobus::VirtualTerminalObjectType;
    switch (object->get_object_type())
    {
        case Type::Container:
        {
            const auto container = std::static_pointer_cast<isobus::Container>(object);
            if (!container->get_hidden())
            {
                render_children(context, object, x, y, depth + 1);
            }
            break;
        }

        case Type::Button:
        {
            const auto button = std::static_pointer_cast<isobus::Button>(object);
            const int width = object->get_width();
            const int height = object->get_height();
            const std::uint16_t background = colour(context, object->get_background_color());
            if (!button->get_option(isobus::Button::Options::TransparentBackground))
            {
                lcd_panel::fill_rect(x, y, width, height, background);
            }
            const int child_offset = button->get_option(isobus::Button::Options::NoBorder) ? 0 : 4;
            if (!button->get_option(isobus::Button::Options::Disabled))
            {
                add_target(context, x, y, width, height, vt_scene_renderer::HitKind::Button, object->get_id(), button->get_key_code());
            }
            render_children(context, object, x + child_offset, y + child_offset, depth + 1);
            // The ISO button border is terminal-proprietary. AgIsoVirtualTerminal
            // renders these controls without an extra rectangular frame; the
            // object-pool children provide any intentional visual outline.
            break;
        }

        case Type::InputBoolean:
        {
            const auto input = std::static_pointer_cast<isobus::InputBoolean>(object);
            const int width = object->get_width();
            const int height = object->get_height();
            const std::uint16_t background = colour(context, object->get_background_color());
            lcd_panel::fill_rect(x, y, width, height, background);

            bool checked = input->get_value() != 0U;
            const auto variable = find_object(context.object_tree, input->get_variable_reference());
            if (variable && (variable->get_object_type() == Type::NumberVariable))
            {
                checked = std::static_pointer_cast<isobus::NumberVariable>(variable)->get_value() != 0U;
            }
            if (checked)
            {
                std::uint16_t foreground = contrasting_text_colour(background);
                const auto foreground_attributes = find_object(context.object_tree,
                                                               input->get_foreground_colour_object_id());
                if (foreground_attributes &&
                    (foreground_attributes->get_object_type() == Type::FontAttributes))
                {
                    foreground = colour(context, foreground_attributes->get_background_color());
                }
                const int thickness = std::max(1, std::min(width, height) / 12);
                lcd_panel::draw_line(x,
                                     y + height / 2,
                                     x + width / 2,
                                     y + height - 1,
                                     foreground,
                                     thickness);
                lcd_panel::draw_line(x + width / 2,
                                     y + height - 1,
                                     x + width - 1,
                                     y,
                                     foreground,
                                     thickness);
            }
            if (input->get_enabled())
            {
                add_target(context, x, y, width, height, vt_scene_renderer::HitKind::InputBoolean, object->get_id());
            }
            break;
        }

        case Type::InputString:
        {
            const auto input = std::static_pointer_cast<isobus::InputString>(object);
            draw_textual_object(context,
                                object,
                                x,
                                y,
                                input->displayed_value(context.object_tree),
                                input->get_font_attributes(),
                                input->get_option(isobus::StringVTObject::Options::Transparent));
            if (input->get_enabled())
            {
                add_target(context, x, y, object->get_width(), object->get_height(), vt_scene_renderer::HitKind::Input, object->get_id());
            }
            break;
        }

        case Type::InputNumber:
        {
            const auto input = std::static_pointer_cast<isobus::InputNumber>(object);
            draw_textual_object(context,
                                object,
                                x,
                                y,
                                format_number(context, input),
                                input->get_font_attributes(),
                                input->get_option(isobus::NumberVTObject::Options::Transparent));
            if (input->get_option2(isobus::InputNumber::Options2::Enabled))
            {
                add_target(context, x, y, object->get_width(), object->get_height(), vt_scene_renderer::HitKind::Input, object->get_id());
            }
            break;
        }

        case Type::InputList:
        {
            const auto list = std::static_pointer_cast<isobus::InputList>(object);
            render_list_object(context,
                               list,
                               x,
                               y,
                               depth,
                               list->get_option(isobus::InputList::Options::Enabled));
            break;
        }

        case Type::OutputString:
        {
            const auto output = std::static_pointer_cast<isobus::OutputString>(object);
            draw_textual_object(context,
                                object,
                                x,
                                y,
                                output->displayed_value(context.object_tree),
                                output->get_font_attributes(),
                                output->get_option(isobus::StringVTObject::Options::Transparent));
            break;
        }

        case Type::OutputNumber:
        {
            const auto output = std::static_pointer_cast<isobus::OutputNumber>(object);
            draw_textual_object(context,
                                object,
                                x,
                                y,
                                format_number(context, output),
                                output->get_font_attributes(),
                                output->get_option(isobus::NumberVTObject::Options::Transparent));
            break;
        }

        case Type::OutputList:
            render_list_object(context,
                               std::static_pointer_cast<isobus::OutputList>(object),
                               x,
                               y,
                               depth,
                               false);
            break;

        case Type::OutputMeter:
            draw_meter(context, std::static_pointer_cast<isobus::OutputMeter>(object), x, y);
            break;

        case Type::OutputLinearBarGraph:
            draw_linear_bar_graph(context, std::static_pointer_cast<isobus::OutputLinearBarGraph>(object), x, y);
            break;

        case Type::OutputArchedBarGraph:
            draw_arched_bar_graph(context, std::static_pointer_cast<isobus::OutputArchedBarGraph>(object), x, y);
            break;

        case Type::OutputRectangle:
        {
            const auto rectangle = std::static_pointer_cast<isobus::OutputRectangle>(object);
            const int width = object->get_width();
            const int height = object->get_height();
            const std::uint16_t background = colour(context, object->get_background_color());
            const std::uint16_t edge_colour = line_colour(context,
                                                          rectangle->get_line_attributes(),
                                                          contrasting_text_colour(background));
            std::uint16_t body_colour = background;
            if (fill_colour(context,
                            rectangle->get_fill_attributes(),
                            edge_colour,
                            background,
                            body_colour))
            {
                lcd_panel::fill_rect(x, y, width, height, body_colour);
            }
            // Every bit in LineArt is a paint/no-paint position. In particular,
            // 0x0000 means that the edge is invisible, which is used around the
            // icon graphics in several Kverneland object pools.
            if (line_art_pattern(context, rectangle->get_line_attributes()) != 0U)
            {
                draw_outline(x,
                             y,
                             width,
                             height,
                             edge_colour,
                             line_width(context, rectangle->get_line_attributes()),
                             rectangle->get_line_suppression_bitfield());
            }
            break;
        }

        case Type::OutputLine:
        {
            const auto line = std::static_pointer_cast<isobus::OutputLine>(object);
            const int width = object->get_width();
            const int height = object->get_height();
            const std::uint16_t line_color = line_colour(context, line->get_line_attributes(), 0xFFFF);
            const int width_pixels = line_width(context, line->get_line_attributes());
            if (line->get_line_direction() == isobus::OutputLine::LineDirection::TopLeftToBottomRight)
            {
                lcd_panel::draw_line(x, y, x + width, y + height, line_color, width_pixels);
            }
            else
            {
                lcd_panel::draw_line(x, y + height, x + width, y, line_color, width_pixels);
            }
            break;
        }

        case Type::OutputEllipse:
            draw_ellipse(context, std::static_pointer_cast<isobus::OutputEllipse>(object), x, y);
            break;

        case Type::OutputPolygon:
            draw_polygon(context, std::static_pointer_cast<isobus::OutputPolygon>(object), x, y);
            break;

        case Type::PictureGraphic:
            draw_picture(context, std::static_pointer_cast<isobus::PictureGraphic>(object), x, y);
            break;

        case Type::ObjectPointer:
        {
            const auto pointer = std::static_pointer_cast<isobus::ObjectPointer>(object);
            render_object(context, find_object(context.object_tree, pointer->get_value()), x, y, depth + 1);
            break;
        }

        default:
            break;
    }
}

void render_children(RenderContext &context,
                     const std::shared_ptr<isobus::VTObject> &parent,
                     int parent_x,
                     int parent_y,
                     int depth)
{
    for (std::uint16_t index = 0; index < parent->get_number_children(); ++index)
    {
        const auto child = find_object(context.object_tree, parent->get_child_id(index));
        render_object(context,
                      child,
                      parent_x + parent->get_child_x(index),
                      parent_y + parent->get_child_y(index),
                      depth);
    }
}

void render_soft_keys(RenderContext &context, const std::shared_ptr<isobus::VTObject> &soft_key_mask)
{
    const auto layout = current_layout();
    static std::uint16_t logged_mask_id = NULL_OBJECT_ID;
    static std::uint16_t logged_visible_designators = 0;
    std::uint16_t visible_designators = 0;
    const std::uint16_t background = colour(context, soft_key_mask->get_background_color());
    lcd_panel::fill_rect(layout.soft_key_x,
                         0,
                         SOFT_KEY_COLUMNS * layout.soft_key_width,
                         layout.data_mask_size,
                         background);

    const std::uint16_t count = std::min<std::uint16_t>(
        soft_key_mask->get_number_children(),
        SOFT_KEY_COLUMNS * SOFT_KEY_ROWS);
    for (std::uint16_t index = 0; index < count; ++index)
    {
        auto key = resolve_object_pointer_chain(
            context.object_tree,
            find_object(context.object_tree, soft_key_mask->get_child_id(index)));
        if ((key == nullptr) || (key->get_object_type() != isobus::VirtualTerminalObjectType::Key))
        {
            continue;
        }

        const int logical_column = index / SOFT_KEY_ROWS;
        const int row = index % SOFT_KEY_ROWS;
        const int visual_column = SOFT_KEY_COLUMNS - 1 - logical_column;
        const int x = layout.soft_key_x + visual_column * layout.soft_key_width;
        const int y = row * layout.soft_key_height;
        const int designator_x = x + (layout.soft_key_width - layout.soft_key_designator_size) / 2;
        const int designator_y = y + (layout.soft_key_height - layout.soft_key_designator_size) / 2;
        const auto key_object = std::static_pointer_cast<isobus::Key>(key);
        for (std::uint16_t child_index = 0; child_index < key->get_number_children(); ++child_index)
        {
            auto child = resolve_object_pointer_chain(
                context.object_tree,
                find_object(context.object_tree, key->get_child_id(child_index)));
            if (child)
            {
                ++visible_designators;
            }
        }
        const std::uint16_t key_background = colour(context, key->get_background_color());
        lcd_panel::fill_rect(x + CONTROL_PADDING,
                             y + CONTROL_PADDING,
                             layout.soft_key_width - 2 * CONTROL_PADDING,
                             layout.soft_key_height - 2 * CONTROL_PADDING,
                             key_background);
        draw_outline(x + CONTROL_PADDING,
                     y + CONTROL_PADDING,
                     layout.soft_key_width - 2 * CONTROL_PADDING,
                     layout.soft_key_height - 2 * CONTROL_PADDING,
                     contrasting_text_colour(key_background));
        add_target(context,
                   x,
                   y,
                   layout.soft_key_width,
                   layout.soft_key_height,
                   vt_scene_renderer::HitKind::SoftKey,
                   key->get_id(),
                   key_object->get_key_code(),
                   context.data_mask_id);
        set_logical_clip(x + CONTROL_PADDING,
                         y + CONTROL_PADDING,
                         layout.soft_key_width - 2 * CONTROL_PADDING,
                         layout.soft_key_height - 2 * CONTROL_PADDING);
        const int inner_screen_x = screen_x(x + CONTROL_PADDING);
        const int inner_screen_y = screen_y(y + CONTROL_PADDING);
        const int inner_screen_width = screen_width(x + CONTROL_PADDING,
                                                    layout.soft_key_width - 2 * CONTROL_PADDING);
        const int inner_screen_height = screen_height(y + CONTROL_PADDING,
                                                      layout.soft_key_height - 2 * CONTROL_PADDING);
        const int designator_screen_size = std::max(1, std::min(inner_screen_width, inner_screen_height));
        const int designator_screen_x = inner_screen_x + (inner_screen_width - designator_screen_size) / 2;
        const int designator_screen_y = inner_screen_y + (inner_screen_height - designator_screen_size) / 2;
        const int designator_offset_x = designator_screen_x -
                                        designator_x * designator_screen_size / layout.soft_key_designator_size;
        const int designator_offset_y = designator_screen_y -
                                        designator_y * designator_screen_size / layout.soft_key_designator_size;
        lcd_panel::set_view_transform(designator_offset_x,
                                      designator_offset_y,
                                      designator_screen_size,
                                      layout.soft_key_designator_size);
        lcd_panel::set_clip_rect(inner_screen_x,
                                 inner_screen_y,
                                 inner_screen_width,
                                 inner_screen_height);
        render_children(context, key, designator_x, designator_y, 1);
        lcd_panel::set_view_transform(layout.screen_x,
                                      0,
                                      layout.scale_numerator,
                                      layout.scale_denominator);
        set_logical_clip(layout.soft_key_x,
                         0,
                         SOFT_KEY_COLUMNS * layout.soft_key_width,
                         layout.data_mask_size);
    }
    if ((logged_mask_id != soft_key_mask->get_id()) ||
        (logged_visible_designators != visible_designators))
    {
        ESP_LOGI(TAG, "Soft key mask %u: %u keys, %u visible designators",
                 soft_key_mask->get_id(),
                 static_cast<unsigned>(count),
                 static_cast<unsigned>(visible_designators));
        logged_mask_id = soft_key_mask->get_id();
        logged_visible_designators = visible_designators;
    }
}

void render_working_set_bar(
    RenderContext &context,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
{
    const auto layout = current_layout();
    lcd_panel::fill_rect(VT_ORIGIN_X,
                         0,
                         layout.working_set_bar_width,
                         layout.data_mask_size,
                         CHROME_DARK);
    const std::uint64_t active_name = context.working_set->get_control_function()
                                          ? context.working_set->get_control_function()->get_NAME().get_full_name()
                                          : 0;
    const std::size_t count = std::min<std::size_t>(
        working_sets.size(),
        layout.data_mask_size / layout.working_set_slot_height);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto &working_set = working_sets[index];
        if (!working_set || !working_set->get_control_function())
        {
            continue;
        }
        const int y = static_cast<int>(index) * layout.working_set_slot_height;
        const std::uint64_t name = working_set->get_control_function()->get_NAME().get_full_name();
        const bool active = name == active_name;
        lcd_panel::fill_rect(VT_ORIGIN_X + CONTROL_PADDING,
                             y + CONTROL_PADDING,
                             layout.working_set_bar_width - 2 * CONTROL_PADDING,
                             layout.working_set_slot_height - 2 * CONTROL_PADDING,
                             CHROME_DARK);
        draw_outline(VT_ORIGIN_X + CONTROL_PADDING,
                     y + CONTROL_PADDING,
                     layout.working_set_bar_width - 2 * CONTROL_PADDING,
                     layout.working_set_slot_height - 2 * CONTROL_PADDING,
                     active ? CHROME_ACTIVE : CHROME_BORDER,
                     active ? 3 : 1);

        const auto working_set_object = working_set->get_working_set_object();
        if (working_set_object)
        {
            RenderContext icon_context = {
                .working_set = working_set,
                .object_tree = working_set->get_object_tree(),
                .data_mask_id = context.data_mask_id,
                .targets = context.targets,
            };
            set_logical_clip(VT_ORIGIN_X + CONTROL_PADDING,
                             y + CONTROL_PADDING,
                             layout.working_set_bar_width - 2 * CONTROL_PADDING,
                             layout.working_set_slot_height - 2 * CONTROL_PADDING);
            const int field_left = screen_x(VT_ORIGIN_X + CONTROL_PADDING);
            const int field_top = screen_y(y + CONTROL_PADDING);
            const int field_width = screen_width(VT_ORIGIN_X + CONTROL_PADDING,
                                                 layout.working_set_bar_width - 2 * CONTROL_PADDING);
            const int field_height = screen_height(y + CONTROL_PADDING,
                                                   layout.working_set_slot_height - 2 * CONTROL_PADDING);
            int icon_left = 0;
            int icon_top = 0;
            int icon_right = std::max(1, layout.soft_key_designator_size);
            int icon_bottom = icon_right;
            // A working-set picture can exceed the nominal soft-key canvas.
            // Fit its displayed dimensions, including the child offset.
            for (std::uint16_t child_index = 0; child_index < working_set_object->get_number_children(); ++child_index)
            {
                const auto child = resolve_object_pointer_chain(icon_context.object_tree,
                    find_object(icon_context.object_tree, working_set_object->get_child_id(child_index)));
                if (child && child->get_object_type() == isobus::VirtualTerminalObjectType::PictureGraphic)
                {
                    const auto picture = std::static_pointer_cast<isobus::PictureGraphic>(child);
                    const int x = working_set_object->get_child_x(child_index);
                    const int y = working_set_object->get_child_y(child_index);
                    icon_left = std::min(icon_left, x);
                    icon_top = std::min(icon_top, y);
                    icon_right = std::max(icon_right, x + static_cast<int>(picture->get_width()));
                    icon_bottom = std::max(icon_bottom, y + static_cast<int>(picture->get_height()));
                }
            }
            const int icon_canvas = std::max(icon_right - icon_left, icon_bottom - icon_top);
            // Keep a small physical-pixel safety margin. Some pools draw exactly on
            // the 60 x 60 designator boundary and bilinear scaling otherwise clips
            // the last row/column (visible as the missing edge of the machine icon).
            constexpr int ICON_SAFE_MARGIN = 10;
            const int icon_scale_numerator = std::max(1, std::min(field_width, field_height) - ICON_SAFE_MARGIN);
            const int icon_scale_denominator = icon_canvas;
            const int drawn_size = icon_canvas * icon_scale_numerator / icon_scale_denominator;
            const int icon_offset_x = field_left + (field_width - drawn_size) / 2;
            const int icon_offset_y = field_top + (field_height - drawn_size) / 2;

            lcd_panel::fill_rect(VT_ORIGIN_X + CONTROL_PADDING,
                                 y + CONTROL_PADDING,
                                 layout.working_set_bar_width - 2 * CONTROL_PADDING,
                                 layout.working_set_slot_height - 2 * CONTROL_PADDING,
                                 colour(icon_context, working_set_object->get_background_color()));
            lcd_panel::set_clip_rect(field_left, field_top, field_width, field_height);
            lcd_panel::set_view_transform_xy(icon_offset_x,
                                             icon_offset_y,
                                             icon_scale_numerator,
                                             icon_scale_denominator,
                                             icon_scale_numerator,
                                             icon_scale_denominator);
            render_children(icon_context, working_set_object,
                            -icon_left + (icon_canvas - (icon_right - icon_left)) / 2,
                            -icon_top + (icon_canvas - (icon_bottom - icon_top)) / 2, 1);
            lcd_panel::set_view_transform(layout.screen_x,
                                          0,
                                          layout.scale_numerator,
                                          layout.scale_denominator);
            set_logical_clip(VT_ORIGIN_X,
                             0,
                             layout.working_set_bar_width,
                             layout.data_mask_size);
        }
        else
        {
            char address[16];
            std::snprintf(address,
                          sizeof(address),
                          "SA %u",
                          static_cast<unsigned>(working_set->get_control_function()->get_address()));
            lcd_panel::draw_text(VT_ORIGIN_X + 18, y + 34, 2, address, CHROME_TEXT);
            lcd_panel::draw_text(VT_ORIGIN_X + 15, y + 58, 1, "BRAK PULI", CHROME_BORDER);
        }

        if (working_set_object)
        {
            context.targets.push_back({
                .x = screen_x(VT_ORIGIN_X),
                .y = screen_y(y),
                .width = screen_width(VT_ORIGIN_X, layout.working_set_bar_width),
                .height = screen_height(y, layout.working_set_slot_height),
                .target = {
                    .kind = vt_scene_renderer::HitKind::WorkingSet,
                    .working_set_name = name,
                },
            });
        }
    }
}

std::shared_ptr<isobus::VTObject> active_data_mask(const ObjectTree &tree,
                                                    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set)
{
    const auto working_set_object = working_set->get_working_set_object();
    if ((working_set_object != nullptr) && (working_set_object->get_object_type() == isobus::VirtualTerminalObjectType::WorkingSet))
    {
        const auto mask = find_object(tree, std::static_pointer_cast<isobus::WorkingSet>(working_set_object)->get_active_mask());
        if ((mask != nullptr) && ((mask->get_object_type() == isobus::VirtualTerminalObjectType::DataMask) ||
                                  (mask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask)))
        {
            return mask;
        }
    }

    for (const auto &[id, object] : tree)
    {
        (void)id;
        if ((object != nullptr) && (object->get_object_type() == isobus::VirtualTerminalObjectType::DataMask))
        {
            return object;
        }
    }
    return nullptr;
}

void add_screen_target(std::vector<ScreenRect> &targets,
                       int x,
                       int y,
                       int width,
                       int height,
                       vt_scene_renderer::TerminalAction action)
{
    targets.push_back({
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .target = {
            .kind = vt_scene_renderer::HitKind::Terminal,
            .terminal_action = action,
        },
    });
}

void draw_screen_button(std::vector<ScreenRect> &targets,
                        int x,
                        int y,
                        int width,
                        int height,
                        const char *label,
                        vt_scene_renderer::TerminalAction action,
                        bool active = false,
                        bool destructive = false)
{
    constexpr std::uint16_t BUTTON = 0x31A6;
    constexpr std::uint16_t BUTTON_ACTIVE = 0x05A8;
    constexpr std::uint16_t BUTTON_DANGER = 0xC1E7;
    const std::uint16_t background = destructive ? BUTTON_DANGER : (active ? BUTTON_ACTIVE : BUTTON);
    lcd_panel::fill_rect(x, y, width, height, background);
    draw_outline(x, y, width, height, active ? CHROME_TEXT : CHROME_BORDER, active ? 3 : 1);
    const int text_width = static_cast<int>(std::strlen(label)) * 12;
    lcd_panel::draw_text(x + std::max(10, (width - text_width) / 2), y + (height - 14) / 2, 2, label, CHROME_TEXT);
    add_screen_target(targets, x, y, width, height, action);
}

void draw_aux_button(std::vector<ScreenRect> &targets,
                     int x,
                     int y,
                     int width,
                     int height,
                     const char *label,
                     vt_scene_renderer::HitKind kind,
                     aux_n::Protocol protocol,
                     std::uint64_t working_set_name,
                     std::uint16_t object_id,
                     bool selected,
                     bool compatible,
                     lcd_panel::TextEncoding encoding = lcd_panel::TextEncoding::ISO8859_1)
{
    const std::uint16_t background = selected ? 0x05A8 : (compatible ? 0x31A6 : 0x7BEF);
    lcd_panel::fill_rect(x, y, width, height, background);
    draw_outline(x, y, width, height, selected ? CHROME_TEXT : CHROME_BORDER, selected ? 3 : 1);
    lcd_panel::draw_text_sized(x + 68, y + (height - 14) / 2, 12, 14, label,
                               compatible ? CHROME_TEXT : 0xBDF7,
                               false, false, false, encoding);
    targets.push_back({
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .target = {
            .kind = kind,
            .object_id = object_id,
            .working_set_name = working_set_name,
            .aux_protocol = static_cast<std::uint8_t>(protocol),
        },
    });
}

struct AuxDesignatorText
{
    std::string value;
    lcd_panel::TextEncoding encoding = lcd_panel::TextEncoding::ISO8859_1;
};

bool find_aux_designator_text(const ObjectTree &tree,
                              const std::shared_ptr<isobus::VTObject> &candidate,
                              AuxDesignatorText &result,
                              std::uint8_t depth = 0)
{
    constexpr std::uint8_t MAX_DEPTH = 16;
    if (!candidate || (depth >= MAX_DEPTH))
    {
        return false;
    }

    const auto object = resolve_object_pointer_chain(tree, candidate);
    if (!object)
    {
        return false;
    }
    if (object->get_object_type() == isobus::VirtualTerminalObjectType::OutputString)
    {
        const auto output = std::static_pointer_cast<isobus::OutputString>(object);
        const auto attributes = find_object(tree, output->get_font_attributes());
        if (attributes && (attributes->get_object_type() == isobus::VirtualTerminalObjectType::FontAttributes))
        {
            switch (std::static_pointer_cast<isobus::FontAttributes>(attributes)->get_type())
            {
                case isobus::FontAttributes::FontType::ISO8859_15: result.encoding = lcd_panel::TextEncoding::ISO8859_15; break;
                case isobus::FontAttributes::FontType::ISO8859_2: result.encoding = lcd_panel::TextEncoding::ISO8859_2; break;
                case isobus::FontAttributes::FontType::ISO8859_4: result.encoding = lcd_panel::TextEncoding::ISO8859_4; break;
                case isobus::FontAttributes::FontType::ISO8859_5: result.encoding = lcd_panel::TextEncoding::ISO8859_5; break;
                case isobus::FontAttributes::FontType::ISO8859_7: result.encoding = lcd_panel::TextEncoding::ISO8859_7; break;
                default: result.encoding = lcd_panel::TextEncoding::ISO8859_1; break;
            }
        }
        result.value = display_text(output->displayed_value(tree), result.encoding);
        while (!result.value.empty() &&
               ((result.value.back() == ' ') || (result.value.back() == '\0') ||
                (result.value.back() == '\r') || (result.value.back() == '\n') || (result.value.back() == '\t')))
        {
            result.value.pop_back();
        }
        if (!result.value.empty())
        {
            return true;
        }
    }

    for (std::uint16_t index = 0; index < object->get_number_children(); ++index)
    {
        if (find_aux_designator_text(tree, find_object(tree, object->get_child_id(index)), result, depth + 1U))
        {
            return true;
        }
    }
    return false;
}

AuxDesignatorText aux_designator_text(
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets,
    std::uint64_t name,
    std::uint16_t object_id)
{
    AuxDesignatorText result;
    const auto working_set = std::find_if(working_sets.begin(), working_sets.end(), [&](const auto &candidate) {
        return candidate && candidate->get_control_function() &&
               (candidate->get_control_function()->get_NAME().get_full_name() == name);
    });
    if (working_set != working_sets.end())
    {
        const auto tree = (*working_set)->get_object_tree();
        find_aux_designator_text(tree, find_object(tree, object_id), result);
    }
    return result;
}

void log_aux_designators_if_changed(
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
{
    const auto catalog = aux_n::get_catalog();
    if (catalog.functions.empty() && catalog.inputs.empty())
    {
        return;
    }

    struct Entry
    {
        bool input;
        std::uint16_t object_id;
        AuxDesignatorText designator;
    };
    std::vector<Entry> entries;
    entries.reserve(catalog.functions.size() + catalog.inputs.size());
    std::string signature;
    for (const auto &function : catalog.functions)
    {
        auto designator = aux_designator_text(working_sets, function.implement_name, function.object_id);
        signature += "F" + std::to_string(function.object_id) + ":" + designator.value + ";";
        entries.push_back({ false, function.object_id, std::move(designator) });
    }
    for (const auto &input : catalog.inputs)
    {
        auto designator = aux_designator_text(working_sets, input.device_name, input.object_id);
        signature += "I" + std::to_string(input.object_id) + ":" + designator.value + ";";
        entries.push_back({ true, input.object_id, std::move(designator) });
    }

    static std::string previous_signature;
    if (signature == previous_signature)
    {
        return;
    }
    previous_signature = std::move(signature);
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        ESP_LOGI(TAG, "AUX %s %u object=%u label='%s'%s",
                 entry.input ? "input" : "function",
                 static_cast<unsigned>(index + 1U),
                 static_cast<unsigned>(entry.object_id),
                 entry.designator.value.empty() ? "" : entry.designator.value.c_str(),
                 entry.designator.value.empty() ? " (fallback)" : "");
    }
}

void draw_aux_designator(
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets,
    std::uint64_t name,
    std::uint16_t object_id,
    int x,
    int y,
    int size)
{
    const auto working_set = std::find_if(working_sets.begin(), working_sets.end(), [&](const auto &candidate) {
        return candidate && candidate->get_control_function() &&
               (candidate->get_control_function()->get_NAME().get_full_name() == name);
    });
    if (working_set == working_sets.end())
    {
        return;
    }
    const auto object = (*working_set)->get_object_by_id(object_id);
    if (!object)
    {
        return;
    }

    std::vector<ScreenRect> ignored_targets;
    RenderContext icon_context = {
        .working_set = *working_set,
        .object_tree = (*working_set)->get_object_tree(),
        .data_mask_id = 0xFFFF,
        .targets = ignored_targets,
    };
    const int canvas = std::max(1, current_layout().soft_key_designator_size);
    lcd_panel::set_clip_rect(x, y, size, size);
    lcd_panel::set_view_transform_xy(x, y, size, canvas, size, canvas);
    render_children(icon_context, object, 0, 0, 1);
    lcd_panel::reset_view_transform();
    lcd_panel::reset_clip_rect();
}

void draw_value_row(int x, int y, int width, const char *label, const char *value, bool emphasized = false)
{
    constexpr std::uint16_t ROW = 0xE71C;
    constexpr std::uint16_t ROW_ALT = 0xD69A;
    constexpr std::uint16_t DARK = 0x1082;
    constexpr std::uint16_t ACCENT = 0x05A8;
    lcd_panel::fill_rect(x, y, width, 48, emphasized ? ROW_ALT : ROW);
    lcd_panel::fill_rect(x, y + 47, width, 1, 0xBDF7);
    const int column_width = width / 2 - 24;
    const int label_scale = static_cast<int>(std::strlen(label)) * 12 > column_width ? 1 : 2;
    const int value_scale = static_cast<int>(std::strlen(value)) * 12 > column_width ? 1 : 2;
    const int value_width = static_cast<int>(std::strlen(value)) * 6 * value_scale;
    lcd_panel::draw_text(x + 16, y + (label_scale == 2 ? 17 : 20), label_scale, label, DARK);
    lcd_panel::draw_text(x + std::max(width / 2, width - value_width - 16),
                         y + (value_scale == 2 ? 17 : 20),
                         value_scale,
                         value,
                         emphasized ? ACCENT : DARK);
}

void draw_page_title(const char *title, const char *subtitle)
{
    constexpr std::uint16_t PAGE = 0xFFFF;
    constexpr std::uint16_t DARK = 0x1082;
    constexpr std::uint16_t MUTED = 0x52AA;
    lcd_panel::fill_rect(LEFT_INFO_WIDTH, 0, board::LCD_WIDTH - LEFT_INFO_WIDTH, board::LCD_HEIGHT, PAGE);
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 28, 24, 3, title, DARK);
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 30, 58, 2, subtitle, MUTED);
    lcd_panel::fill_rect(LEFT_INFO_WIDTH + 28, 88, board::LCD_WIDTH - LEFT_INFO_WIDTH - 56, 3, 0x3C86);
}

void render_machine_page(
    std::vector<ScreenRect> &targets,
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
{
    draw_page_title("MASZYNY", "STEROWNIKI VT I ZAPISANE PULE OBIEKTOW");
    const std::uint64_t active_name = working_set_name(active_working_set);
    int y = 112;
    for (const auto &working_set : working_sets)
    {
        if (!working_set || !working_set->get_control_function() || (y > 380))
        {
            continue;
        }
        const auto control_function = working_set->get_control_function();
        const std::uint64_t name = control_function->get_NAME().get_full_name();
        const bool active = name == active_name;
        lcd_panel::fill_rect(LEFT_INFO_WIDTH + 28, y, 760, 66, active ? 0xCFF3 : 0xE71C);
        draw_outline(LEFT_INFO_WIDTH + 28, y, 760, 66, active ? 0x05A8 : 0xBDF7, active ? 3 : 1);
        char title[56];
        char detail[72];
        std::snprintf(title, sizeof(title), "%sSTEROWNIK SA %u", active ? "AKTYWNY  " : "", static_cast<unsigned>(control_function->get_address()));
        std::snprintf(detail, sizeof(detail), "NAME %016llX   ZAPISANE WERSJE %u",
                      static_cast<unsigned long long>(name),
                      static_cast<unsigned>(object_pool_store::list_versions(name).size()));
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 46, y + 12, 2, title, 0x1082);
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 46, y + 38, 2, detail, 0x52AA);
        targets.push_back({
            .x = LEFT_INFO_WIDTH + 28,
            .y = y,
            .width = 760,
            .height = 66,
            .target = {
                .kind = vt_scene_renderer::HitKind::WorkingSet,
                .working_set_name = name,
            },
        });
        y += 78;
    }
    if (working_sets.empty())
    {
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 44, 142, 2, "BRAK STEROWNIKOW OCZEKUJACYCH NA VT", 0x52AA);
    }

    const auto storage = object_pool_store::get_statistics();
    char storage_text[72];
    std::snprintf(storage_text, sizeof(storage_text), "PAMIEC: %u PUL   %u KB WOLNE",
                  static_cast<unsigned>(storage.object_pool_count),
                  static_cast<unsigned>((storage.total_bytes - std::min(storage.total_bytes, storage.used_bytes)) / 1024U));
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 36, 434, 2, storage_text, 0x52AA);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 478, 360, 72, "USUN PULE AKTYWNEJ", vt_scene_renderer::TerminalAction::EraseActivePool, false, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 416, 478, 372, 72, "USUN WSZYSTKIE PULE", vt_scene_renderer::TerminalAction::EraseAllPools, false, true);
}

void render_terminal_page(std::vector<ScreenRect> &targets)
{
    draw_page_title("TERMINAL", "USTAWIENIA PRZEKAZYWANE MASZYNOM PRZEZ ISOBUS");
    const char *language = "JEZYK: POLSKI   KRAJ: PL";
    switch (terminal_settings.language)
    {
        case vt_scene_renderer::TerminalLanguage::English:
            language = "JEZYK: ENGLISH   KRAJ: GB";
            break;
        case vt_scene_renderer::TerminalLanguage::German:
            language = "JEZYK: DEUTSCH   KRAJ: DE";
            break;
        case vt_scene_renderer::TerminalLanguage::French:
            language = "JEZYK: FRANCAIS   KRAJ: FR";
            break;
        case vt_scene_renderer::TerminalLanguage::Hungarian:
            language = "JEZYK: MAGYAR   KRAJ: HU";
            break;
        case vt_scene_renderer::TerminalLanguage::Dutch:
            language = "JEZYK: NEDERLANDS   KRAJ: NL";
            break;
        case vt_scene_renderer::TerminalLanguage::Russian:
            language = "JEZYK: RUSSIAN   KRAJ: RU";
            break;
        case vt_scene_renderer::TerminalLanguage::Lithuanian:
            language = "JEZYK: LIETUVIU   KRAJ: LT";
            break;
        default:
            break;
    }
    const char *date_format = "DATA: DD/MM/RRRR";
    if (terminal_settings.date_format == vt_scene_renderer::TerminalDateFormat::MonthDayYear)
    {
        date_format = "DATA: MM/DD/RRRR";
    }
    else if (terminal_settings.date_format == vt_scene_renderer::TerminalDateFormat::YearMonthDay)
    {
        date_format = "DATA: RRRR-MM-DD";
    }

    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 86, 760, 52, language,
                       vt_scene_renderer::TerminalAction::CycleLanguage, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 142, 760, 52,
                       terminal_settings.metric_units ? "JEDNOSTKI: METRYCZNE" : "JEDNOSTKI: IMPERIALNE",
                       vt_scene_renderer::TerminalAction::ToggleUnits, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 198, 760, 52,
                       terminal_settings.decimal_comma ? "SEPARATOR DZIESIETNY: PRZECINEK" : "SEPARATOR DZIESIETNY: KROPKA",
                       vt_scene_renderer::TerminalAction::ToggleDecimalSymbol, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 254, 760, 52,
                       terminal_settings.twenty_four_hour ? "FORMAT CZASU: 24 H" : "FORMAT CZASU: 12 H AM/PM",
                       vt_scene_renderer::TerminalAction::ToggleTimeFormat, true);
    draw_screen_button(targets,
                       LEFT_INFO_WIDTH + 28,
                       310,
                       760,
                       52,
                       date_format,
                       vt_scene_renderer::TerminalAction::CycleDateFormat,
                       true);
    char terminal_number[48];
    std::snprintf(terminal_number,
                  sizeof(terminal_number),
                  "NUMER TERMINALA: %u (1-32)",
                  static_cast<unsigned>(terminal_settings.terminal_number));
    draw_screen_button(targets,
                       LEFT_INFO_WIDTH + 28,
                       366,
                       760,
                       52,
                       terminal_number,
                       vt_scene_renderer::TerminalAction::CycleTerminalNumber,
                       true);
    char data_mask_size[48];
    std::snprintf(data_mask_size,
                  sizeof(data_mask_size),
                  "OBSZAR ROBOCZY: %u X %u",
                  static_cast<unsigned>(terminal_settings.data_mask_size),
                  static_cast<unsigned>(terminal_settings.data_mask_size));
    draw_screen_button(targets,
                       LEFT_INFO_WIDTH + 28,
                       422,
                       760,
                       52,
                       data_mask_size,
                       vt_scene_renderer::TerminalAction::CycleDataMaskSize,
                       true);
    char capabilities[64];
    std::snprintf(capabilities,
                  sizeof(capabilities),
                  "VT %u   WERSJA 6   %u X %u   12 KLAWISZY",
                  static_cast<unsigned>(terminal_settings.terminal_number),
                  static_cast<unsigned>(terminal_settings.data_mask_size),
                  static_cast<unsigned>(terminal_settings.data_mask_size));
    draw_value_row(LEFT_INFO_WIDTH + 28, 482, 760, "TERMINAL", capabilities);
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 42, 544, 2, "NUMER VT I OBSZAR ROBOCZY WYMAGAJA RESTARTU", 0x52AA);
}

void render_isobus_page(std::vector<ScreenRect> &targets)
{
    draw_page_title("ISOBUS", "DIAGNOSTYKA SIECI WYSWIETLANA NA TERMINALU");
    const auto members = isobus::CANNetworkManager::CANNetwork.get_control_functions(false);
    char value[48];
    std::snprintf(value, sizeof(value), "%.1f %%", isobus::CANNetworkManager::CANNetwork.get_estimated_busload(0));
    draw_value_row(LEFT_INFO_WIDTH + 28, 112, 365, "MAGISTRALA", "CAN 250 KBIT/S", true);
    draw_value_row(LEFT_INFO_WIDTH + 423, 112, 365, "OBCIAZENIE", value, true);
    std::snprintf(value, sizeof(value), "%u", static_cast<unsigned>(members.size()));
    draw_value_row(LEFT_INFO_WIDTH + 28, 160, 365, "WEZLY ONLINE", value);
    draw_value_row(LEFT_INFO_WIDTH + 423, 160, 365, "VT ADDRESS", "38 / 0X26");

    int y = 230;
    std::size_t shown = 0;
    for (const auto &member : members)
    {
        if (!member || (shown >= 5))
        {
            continue;
        }
        const auto name = member->get_NAME();
        char line[96];
        std::snprintf(line, sizeof(line), "SA %3u   FUNKCJA %3u   PROD %4u   NAME %016llX",
                      static_cast<unsigned>(member->get_address()),
                      static_cast<unsigned>(name.get_function_code()),
                      static_cast<unsigned>(name.get_manufacturer_code()),
                      static_cast<unsigned long long>(name.get_full_name()));
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 38, y, 2, line, 0x1082);
        y += 42;
        ++shown;
    }
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 480, 760, 70, "ODSWIEZ URZADZENIA", vt_scene_renderer::TerminalAction::RefreshNetwork, true);
}

void render_aux_page(std::vector<ScreenRect> &targets,
                     const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
                     const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets)
{
    const auto function_type_label = [](std::uint8_t function_type) -> const char * {
        switch (static_cast<aux_n::FunctionType>(function_type))
        {
            case aux_n::FunctionType::BooleanLatching: return "WL/WYL";
            case aux_n::FunctionType::AnalogueLatching: return "ANALOG STALY";
            case aux_n::FunctionType::BooleanMomentary: return "PRZYCISK";
            case aux_n::FunctionType::AnalogueMomentaryTwoWay: return "ANALOG 2-KIER";
            case aux_n::FunctionType::AnalogueMomentaryOneWay: return "ANALOG 1-KIER";
            case aux_n::FunctionType::DualBooleanLatching: return "2X WL/WYL";
            case aux_n::FunctionType::DualBooleanMomentary: return "2X PRZYCISK";
            case aux_n::FunctionType::DualBooleanLatchingUpOnly: return "2X GORA";
            case aux_n::FunctionType::DualBooleanLatchingDownOnly: return "2X DOL";
            case aux_n::FunctionType::AnalogueMomentaryBooleanLatching: return "ANALOG+WL/WYL";
            case aux_n::FunctionType::AnalogueLatchingBooleanLatching: return "AN.STALY+WL/WYL";
            case aux_n::FunctionType::QuadratureBooleanMomentary: return "KWADR PRZYCISK";
            case aux_n::FunctionType::QuadratureAnalogueLatching: return "KWADR ANALOG ST";
            case aux_n::FunctionType::QuadratureAnalogueMomentary: return "KWADR ANALOG";
            case aux_n::FunctionType::BidirectionalEncoder: return "ENKODER 2-KIER";
            default: return "NIEZNANY";
        }
    };

    aux_n::set_legacy_type1_enabled(terminal_settings.aux_o_legacy);
    const auto catalog = aux_n::get_catalog();
    draw_page_title("AUX", terminal_settings.aux_o_legacy ? "TRYB ZGODNOSCI AUX-O / VT VERSION 2" : "AUX-N TYPE 2 / ISO 11783-6 VT VERSION 6");

    char header[64];
    std::snprintf(header, sizeof(header), "FUNKCJE MASZYNY: %u", static_cast<unsigned>(catalog.functions.size()));
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 28, 90, 2, header, 0x1082);
    std::snprintf(header, sizeof(header), "WEJSCIA: %u", static_cast<unsigned>(catalog.inputs.size()));
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 420, 90, 2, header, 0x1082);
    const std::size_t function_page_count = std::max<std::size_t>(1, (catalog.functions.size() + 4U) / 5U);
    const std::size_t input_page_count = std::max<std::size_t>(1, (catalog.inputs.size() + 4U) / 5U);
    if (aux_function_page >= function_page_count)
    {
        aux_function_page = function_page_count - 1U;
    }
    if (aux_input_page >= input_page_count)
    {
        aux_input_page = input_page_count - 1U;
    }
    char page_label[24];
    if (function_page_count > 1U)
    {
        std::snprintf(page_label, sizeof(page_label), "%u/%u", static_cast<unsigned>(aux_function_page + 1U), static_cast<unsigned>(function_page_count));
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 350, 90, 2, page_label, 0x1082);
        draw_screen_button(targets, LEFT_INFO_WIDTH + 286, 82, 30, 32, "<", vt_scene_renderer::TerminalAction::AuxFunctionPreviousPage);
        draw_screen_button(targets, LEFT_INFO_WIDTH + 322, 82, 30, 32, ">", vt_scene_renderer::TerminalAction::AuxFunctionNextPage);
    }
    if (input_page_count > 1U)
    {
        std::snprintf(page_label, sizeof(page_label), "%u/%u", static_cast<unsigned>(aux_input_page + 1U), static_cast<unsigned>(input_page_count));
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 716, 90, 2, page_label, 0x1082);
        draw_screen_button(targets, LEFT_INFO_WIDTH + 640, 82, 30, 32, "<", vt_scene_renderer::TerminalAction::AuxInputPreviousPage);
        draw_screen_button(targets, LEFT_INFO_WIDTH + 676, 82, 30, 32, ">", vt_scene_renderer::TerminalAction::AuxInputNextPage);
    }

    int y = 124;
    std::size_t shown = 0;
    for (std::size_t index = aux_function_page * 5U; index < catalog.functions.size(); ++index)
    {
        const auto &function = catalog.functions[index];
        if (shown++ >= 5)
        {
            break;
        }
        char line[48];
        const auto assignment = std::find_if(catalog.assignments.begin(), catalog.assignments.end(), [&](const aux_n::Assignment &entry) {
            return (entry.protocol == function.protocol) && (entry.implement_name == function.implement_name) &&
                   (entry.function_object_id == function.object_id);
        });
        const auto designator = aux_designator_text(working_sets, function.implement_name, function.object_id);
        const char *assignment_label = assignment == catalog.assignments.end() ? "" : (assignment->confirmed ? "  OK" : "  ...");
        if (!designator.value.empty())
        {
            std::snprintf(line, sizeof(line), "%.20s%s", designator.value.c_str(), assignment_label);
        }
        else
        {
            std::snprintf(line, sizeof(line), "F%u %s%s",
                          static_cast<unsigned>(index + 1U),
                          function_type_label(function.function_type),
                          assignment_label);
        }
        const bool selected = (catalog.selected_protocol == function.protocol) &&
                              (catalog.selected_implement_name == function.implement_name) &&
                              (catalog.selected_function_object_id == function.object_id);
        draw_aux_button(targets, LEFT_INFO_WIDTH + 28, y, 360, 58, line,
                        vt_scene_renderer::HitKind::AuxFunction, function.protocol,
                        function.implement_name, function.object_id, selected, true, designator.encoding);
        draw_aux_designator(working_sets, function.implement_name, function.object_id,
                            LEFT_INFO_WIDTH + 36, y + 6, 46);
        y += 66;
    }

    y = 124;
    shown = 0;
    for (std::size_t index = aux_input_page * 5U; index < catalog.inputs.size(); ++index)
    {
        const auto &input = catalog.inputs[index];
        if (shown++ >= 5)
        {
            break;
        }
        const auto selected_function = std::find_if(catalog.functions.begin(), catalog.functions.end(), [&](const aux_n::FunctionDescriptor &function) {
            return (function.protocol == catalog.selected_protocol) &&
                   (function.implement_name == catalog.selected_implement_name) &&
                   (function.object_id == catalog.selected_function_object_id);
        });
        const bool compatible = (selected_function != catalog.functions.end()) &&
                                (selected_function->protocol == input.protocol) &&
                                (selected_function->function_type == input.function_type) &&
                                (!selected_function->critical || input.critical);
        char line[48];
        const auto designator = aux_designator_text(working_sets, input.device_name, input.object_id);
        char fallback_label[48];
        std::snprintf(fallback_label, sizeof(fallback_label), "B%u %s",
                      static_cast<unsigned>(index + 1U), function_type_label(input.function_type));
        const char *input_label = designator.value.empty() ? fallback_label : designator.value.c_str();
        const auto status = std::find_if(catalog.input_statuses.begin(), catalog.input_statuses.end(), [&](const aux_n::InputStatus &entry) {
            return (entry.device_name == input.device_name) && (entry.input_object_id == input.object_id);
        });
        if (status != catalog.input_statuses.end())
        {
            std::snprintf(line, sizeof(line), "%.12s  %u/%u",
                          input_label,
                          static_cast<unsigned>(status->value_1), static_cast<unsigned>(status->value_2));
        }
        else
        {
            std::snprintf(line, sizeof(line), "%.18s%s", input_label, input.online ? " ONLINE" : "");
        }
        draw_aux_button(targets, LEFT_INFO_WIDTH + 420, y, 368, 58, line,
                        vt_scene_renderer::HitKind::AuxInput, input.protocol,
                        input.device_name, input.object_id, false, compatible, designator.encoding);
        draw_aux_designator(working_sets, input.device_name, input.object_id,
                            LEFT_INFO_WIDTH + 428, y + 6, 46);
        y += 66;
    }

    constexpr int bottom_y = 474;
    constexpr int button_width = 184;
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, bottom_y, button_width, 70,
                       terminal_settings.aux_o_legacy ? "AUX-O VT2" : "AUX-N VT6",
                       vt_scene_renderer::TerminalAction::ToggleAuxOLegacy,
                       terminal_settings.aux_o_legacy);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 220, bottom_y, button_width, 70,
                       catalog.learning ? "UCZENIE..." : "UCZ PRZYCISK",
                       vt_scene_renderer::TerminalAction::ToggleAuxLearn, catalog.learning);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 412, bottom_y, button_width, 70, "USUN WYBRANE",
                       vt_scene_renderer::TerminalAction::UnassignSelectedAux, false, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 604, bottom_y, button_width, 70, "USUN WSZYSTKIE",
                       vt_scene_renderer::TerminalAction::ClearAuxAssignments, false, true);
}

void render_gnss_page()
{
    draw_page_title("GNSS I SEKCJE", "STAN ODBIORNIKA, NTRIP I BLOKADY AUTOMATYKI");
    const auto gnss = gnss_um982::get_snapshot();
    const auto sections = section_control::get_snapshot();
#if CONFIG_ISOVT_GNSS_SOURCE_UM982
    constexpr const char *SOURCE = "UM982 UART";
#elif CONFIG_ISOVT_GNSS_SOURCE_RS232
    constexpr const char *SOURCE = "NMEA RS232";
#else
    constexpr const char *SOURCE = "MODUL WYLACZONY";
#endif
    draw_value_row(LEFT_INFO_WIDTH + 28, 112, 760, "ZRODLO", SOURCE, gnss.receiver_configured);
    draw_value_row(LEFT_INFO_WIDTH + 28, 160, 760, "FIX", gnss_um982::fix_quality_text(gnss.fix_quality), gnss.position_valid);
    char value[64];
    std::snprintf(value, sizeof(value), "%u   HDOP %.2f", static_cast<unsigned>(gnss.satellites), gnss.hdop);
    draw_value_row(LEFT_INFO_WIDTH + 28, 208, 760, "SATELITY", value);
    std::snprintf(value, sizeof(value), "%.2f KM/H   %.1f DEG", gnss.speed_mps * 3.6F, gnss.course_degrees);
    draw_value_row(LEFT_INFO_WIDTH + 28, 256, 760, "PREDKOSC / KIERUNEK", value);
    std::snprintf(value, sizeof(value), "%.7f   %.7f", gnss.latitude_degrees, gnss.longitude_degrees);
    draw_value_row(LEFT_INFO_WIDTH + 28, 304, 760, "POZYCJA", value);
    draw_value_row(LEFT_INFO_WIDTH + 28, 352, 760, "NTRIP", gnss.ntrip_enabled ? (gnss.ntrip_connected ? "POLACZONY" : "OCZEKIWANIE") : "WYLACZONY");
#if CONFIG_ISOVT_SECTION_CONTROL_ALLOW_RTK_FLOAT
    constexpr const char *FIX_GATE = "RTK FLOAT LUB FIX";
#else
    constexpr const char *FIX_GATE = "TYLKO RTK FIX";
#endif
    draw_value_row(LEFT_INFO_WIDTH + 28, 400, 760, "MINIMUM DLA SEKCJI", FIX_GATE);
    std::snprintf(value, sizeof(value), "%s   %u SEKCJI   WYJSCIA ZABLOKOWANE",
                  section_control::gate_state_text(sections.gate_state),
                  static_cast<unsigned>(sections.configured_sections));
    draw_value_row(LEFT_INFO_WIDTH + 28, 448, 760, "AUTOMATYKA", value, sections.gate_state == section_control::GateState::Ready);
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 42, 526, 2, "WYJSCIA POZOSTAJA ZABLOKOWANE DO IMPLEMENTACJI TC-SC", 0x52AA);
}

void render_audio_page(std::vector<ScreenRect> &targets)
{
    draw_page_title("DZWIEK", "GLOSNOSC SYGNALOW TERMINALA I ALARMOW MASZYNY");

    char value[32];
    std::snprintf(value, sizeof(value), "%u %%", static_cast<unsigned>(terminal_settings.control_volume));
    draw_value_row(LEFT_INFO_WIDTH + 28, 112, 480, "SYGNALY ROBOCZE", value, !terminal_settings.audio_muted);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 530, 112, 120, 64, "-", vt_scene_renderer::TerminalAction::DecreaseControlVolume, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 668, 112, 120, 64, "+", vt_scene_renderer::TerminalAction::IncreaseControlVolume, true);

    std::snprintf(value, sizeof(value), "%u %%", static_cast<unsigned>(terminal_settings.alarm_volume));
    draw_value_row(LEFT_INFO_WIDTH + 28, 202, 480, "ALARMY", value, !terminal_settings.audio_muted);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 530, 202, 120, 64, "-", vt_scene_renderer::TerminalAction::DecreaseAlarmVolume, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 668, 202, 120, 64, "+", vt_scene_renderer::TerminalAction::IncreaseAlarmVolume, true);

    draw_screen_button(targets,
                       LEFT_INFO_WIDTH + 28,
                       302,
                       760,
                       70,
                       terminal_settings.audio_muted ? "DZWIEK WYCISZONY - WLACZ" : "DZWIEK WLACZONY - WYCISZ",
                       vt_scene_renderer::TerminalAction::ToggleAudioMute,
                       !terminal_settings.audio_muted,
                       true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 28, 396, 365, 76, "TEST SYGNALU", vt_scene_renderer::TerminalAction::TestControlAudio, true);
    draw_screen_button(targets, LEFT_INFO_WIDTH + 423, 396, 365, 76, "TEST ALARMU", vt_scene_renderer::TerminalAction::TestAlarmAudio, true);
    lcd_panel::draw_text(LEFT_INFO_WIDTH + 42, 514, 2, "ZAKRES 0-100 %, KROK 5 %. USTAWIENIA SA ZAPAMIETYWANE.", 0x52AA);
}

void render_information_page()
{
    draw_page_title("INFORMACJE", "MOZLIWOSCI I STAN TERMINALA");
    const esp_app_desc_t *app = esp_app_get_description();
    draw_value_row(LEFT_INFO_WIDTH + 28, 112, 760, "OPROGRAMOWANIE", app ? app->project_name : "ESP ISOBUS VT", true);
    draw_value_row(LEFT_INFO_WIDTH + 28, 160, 760, "WERSJA VT",
                   terminal_settings.aux_o_legacy ? "ISO 11783-6 VT VERSION 2" : "ISO 11783-6 VT VERSION 6");
    draw_value_row(LEFT_INFO_WIDTH + 28, 208, 760, "EKRAN", "1024 X 600 DOTYKOWY");
    draw_value_row(LEFT_INFO_WIDTH + 28, 256, 760, "OBSZAR / SOFTKEY", "480 X 480 / 12 KLAWISZY");
    draw_value_row(LEFT_INFO_WIDTH + 28, 304, 760, "AUX",
                   terminal_settings.aux_o_legacy ? "AUX-O TYPE 1 (LEGACY)" : "AUX-N TYPE 2");
    draw_value_row(LEFT_INFO_WIDTH + 28, 352, 760, "PAMIEC PUL", "SPIFFS FLASH");
    char heap[32];
    std::snprintf(heap, sizeof(heap), "%u KB", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024U));
    draw_value_row(LEFT_INFO_WIDTH + 28, 400, 760, "WOLNA PAMIEC RAM", heap);
    draw_value_row(LEFT_INFO_WIDTH + 28, 448, 760, "DIAGNOSTYKA", "TYLKO NA EKRANIE");
}

void render_terminal_menu(
    std::vector<ScreenRect> &targets,
    const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &active_working_set,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets,
    const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &auxiliary_working_sets)
{
    lcd_panel::fill(CHROME_DARK);
    lcd_panel::fill_rect(0, 0, LEFT_INFO_WIDTH, board::LCD_HEIGHT, CHROME_DARK);
    lcd_panel::draw_text(18, 18, 3, "ISOBUS VT", CHROME_TEXT);
    lcd_panel::draw_text(18, 52, 2, "MENU TERMINALA", CHROME_BORDER);

    struct NavigationItem
    {
        const char *label;
        vt_scene_renderer::TerminalAction action;
    };
    constexpr NavigationItem ITEMS[] = {
        { "MASZYNY", vt_scene_renderer::TerminalAction::ShowMachines },
        { "TERMINAL", vt_scene_renderer::TerminalAction::ShowTerminal },
        { "ISOBUS", vt_scene_renderer::TerminalAction::ShowIsobus },
        { "AUX-N", vt_scene_renderer::TerminalAction::ShowAuxN },
        { "GNSS / SEKCJE", vt_scene_renderer::TerminalAction::ShowGnss },
        { "DZWIEK", vt_scene_renderer::TerminalAction::ShowAudio },
        { "INFORMACJE", vt_scene_renderer::TerminalAction::ShowInformation },
    };
    int y = 88;
    for (const auto &item : ITEMS)
    {
        draw_screen_button(targets, 12, y, LEFT_INFO_WIDTH - 24, 50, item.label, item.action, terminal_page == item.action);
        y += 58;
    }
    draw_screen_button(targets, 12, 506, LEFT_INFO_WIDTH - 24, 76, "POWROT", vt_scene_renderer::TerminalAction::ToggleMenu, true);

    switch (terminal_page)
    {
        case vt_scene_renderer::TerminalAction::ShowTerminal:
            render_terminal_page(targets);
            break;
        case vt_scene_renderer::TerminalAction::ShowIsobus:
            render_isobus_page(targets);
            break;
        case vt_scene_renderer::TerminalAction::ShowAuxN:
            render_aux_page(targets, active_working_set, auxiliary_working_sets);
            break;
        case vt_scene_renderer::TerminalAction::ShowGnss:
            render_gnss_page();
            break;
        case vt_scene_renderer::TerminalAction::ShowAudio:
            render_audio_page(targets);
            break;
        case vt_scene_renderer::TerminalAction::ShowInformation:
            render_information_page();
            break;
        default:
            render_machine_page(targets, active_working_set, working_sets);
            break;
    }

    if (!terminal_notice.empty())
    {
        lcd_panel::fill_rect(LEFT_INFO_WIDTH + 28, 560, board::LCD_WIDTH - LEFT_INFO_WIDTH - 56, 30, CHROME_DARK);
        lcd_panel::draw_text(LEFT_INFO_WIDTH + 42, 568, 2, terminal_notice.c_str(), CHROME_TEXT);
    }
}

void add_editor_target(std::vector<ScreenRect> &targets,
                       int x,
                       int y,
                       int width,
                       int height,
                       vt_scene_renderer::EditorAction action,
                       char character = '\0',
                       std::uint16_t index = 0)
{
    targets.push_back({
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .target = {
            .kind = vt_scene_renderer::HitKind::Editor,
            .editor_action = action,
            .editor_character = character,
            .editor_index = index,
        },
    });
}

void draw_editor_key(std::vector<ScreenRect> &targets,
                     int x,
                     int y,
                     int width,
                     int height,
                     const char *label,
                     vt_scene_renderer::EditorAction action,
                     char character = '\0',
                     std::uint16_t index = 0,
                     bool active = false,
                     lcd_panel::TextEncoding encoding = lcd_panel::TextEncoding::ISO8859_1)
{
    const std::uint16_t background = active ? CHROME_ACTIVE : 0xDEFB;
    lcd_panel::fill_rect(x, y, width, height, background);
    draw_outline(x, y, width, height, active ? CHROME_TEXT : CHROME_DARK, 2);
    const int text_scale = (std::strlen(label) <= 2) ? 3 : 2;
    const int text_width = static_cast<int>(std::strlen(label)) * 6 * text_scale;
    lcd_panel::draw_text_sized(x + std::max(6, (width - text_width) / 2),
                               y + std::max(4, (height - 7 * text_scale) / 2),
                               6 * text_scale,
                               7 * text_scale,
                               label,
                               active ? CHROME_TEXT : CHROME_DARK,
                               false,
                               false,
                               false,
                               encoding);
    add_editor_target(targets, x, y, width, height, action, character, index);
}

lcd_panel::TextEncoding editor_encoding(vt_scene_renderer::EditorTextEncoding encoding)
{
    switch (encoding)
    {
        case vt_scene_renderer::EditorTextEncoding::ISO8859_15:
            return lcd_panel::TextEncoding::ISO8859_15;
        case vt_scene_renderer::EditorTextEncoding::ISO8859_2:
            return lcd_panel::TextEncoding::ISO8859_2;
        case vt_scene_renderer::EditorTextEncoding::ISO8859_4:
            return lcd_panel::TextEncoding::ISO8859_4;
        case vt_scene_renderer::EditorTextEncoding::ISO8859_5:
            return lcd_panel::TextEncoding::ISO8859_5;
        case vt_scene_renderer::EditorTextEncoding::ISO8859_7:
            return lcd_panel::TextEncoding::ISO8859_7;
        default:
            return lcd_panel::TextEncoding::ISO8859_1;
    }
}

void render_input_editor(std::vector<ScreenRect> &targets, RenderContext *context = nullptr)
{
    InputEditorState editor;
    {
        std::lock_guard<std::mutex> lock(input_editor_mutex);
        editor = input_editor;
    }
    if (!editor.open)
    {
        return;
    }

    targets.clear();
    lcd_panel::reset_view_transform();
    lcd_panel::reset_clip_rect();
    lcd_panel::fill(CHROME_DARK);
    constexpr int panel_x = 54;
    constexpr int panel_y = 24;
    constexpr int panel_width = 916;
    constexpr int panel_height = 552;
    lcd_panel::fill_rect(panel_x, panel_y, panel_width, panel_height, 0xFFFF);
    draw_outline(panel_x, panel_y, panel_width, panel_height, CHROME_BORDER, 3);

    const char *title = editor.kind == vt_scene_renderer::InputEditorKind::Number ? "WPROWADZ LICZBE" :
                        editor.kind == vt_scene_renderer::InputEditorKind::Text ? "WPROWADZ TEKST" :
                                                                                 "WYBIERZ POZYCJE";
    lcd_panel::fill_rect(panel_x, panel_y, panel_width, 58, CHROME_BACKGROUND);
    lcd_panel::draw_text(panel_x + 24, panel_y + 17, 3, title, CHROME_TEXT);

    if (editor.kind == vt_scene_renderer::InputEditorKind::List)
    {
        const int visible_rows = 6;
        const int first = std::clamp<int>(static_cast<int>(editor.selected_index) - visible_rows / 2,
                                          0,
                                          std::max(0, static_cast<int>(editor.list_items.size()) - visible_rows));
        for (int row = 0; row < visible_rows; ++row)
        {
            const int index = first + row;
            if (index >= static_cast<int>(editor.list_items.size()))
            {
                break;
            }
            const int y = panel_y + 76 + row * 66;
            const auto &item = editor.list_items[static_cast<std::size_t>(index)];
            draw_editor_key(targets,
                            panel_x + 26,
                            y,
                            panel_width - 192,
                            56,
                            item.label.c_str(),
                            vt_scene_renderer::EditorAction::Select,
                            '\0',
                            static_cast<std::uint16_t>(index),
                            index == editor.selected_index,
                            editor_encoding(item.encoding));
            if ((item.object_id != NULL_OBJECT_ID) && (context != nullptr))
            {
                auto item_object = find_object(context->object_tree, item.object_id);
                auto measured_object = resolve_object_pointer_chain(context->object_tree, item_object);
                if (item_object && measured_object)
                {
                    std::vector<ScreenRect> ignored_targets;
                    RenderContext item_context = {
                        .working_set = context->working_set,
                        .object_tree = context->object_tree,
                        .data_mask_id = context->data_mask_id,
                        .targets = ignored_targets,
                    };
                    const int item_width = std::max(1, static_cast<int>(measured_object->get_width()));
                    const int item_height = std::max(1, static_cast<int>(measured_object->get_height()));
                    const int item_x = panel_x + 26 + std::max(8, (panel_width - 192 - item_width) / 2);
                    const int item_y = y + std::max(2, (56 - item_height) / 2);
                    lcd_panel::set_clip_rect(panel_x + 28, y + 2, panel_width - 196, 52);
                    render_object(item_context, item_object, item_x, item_y, 1);
                    lcd_panel::reset_clip_rect();
                }
            }
        }
        draw_editor_key(targets, panel_x + panel_width - 142, panel_y + 92, 112, 72, "GORA", vt_scene_renderer::EditorAction::Previous);
        draw_editor_key(targets, panel_x + panel_width - 142, panel_y + 178, 112, 72, "DOL", vt_scene_renderer::EditorAction::Next);
        draw_editor_key(targets, panel_x + panel_width - 142, panel_y + 350, 112, 72, "ANULUJ", vt_scene_renderer::EditorAction::Cancel);
        draw_editor_key(targets, panel_x + panel_width - 142, panel_y + 436, 112, 72, "OK", vt_scene_renderer::EditorAction::Confirm, '\0', 0, true);
        return;
    }

    lcd_panel::fill_rect(panel_x + 24, panel_y + 74, panel_width - 48, 64, 0xFFFF);
    draw_outline(panel_x + 24, panel_y + 74, panel_width - 48, 64, CHROME_DARK, 2);
    const std::string shown_value = editor.value.empty() ? " " : editor.value;
    lcd_panel::draw_text_sized(panel_x + 40,
                               panel_y + 94,
                               18,
                               21,
                               shown_value.c_str(),
                               CHROME_DARK,
                               false,
                               false,
                               false,
                               editor_encoding(editor.text_encoding));

    if (editor.kind == vt_scene_renderer::InputEditorKind::Number)
    {
        constexpr const char *keys[] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", "-", "0", "." };
        for (int index = 0; index < 12; ++index)
        {
            const int column = index % 3;
            const int row = index / 3;
            const char character = keys[index][0];
            draw_editor_key(targets,
                            panel_x + 24 + column * 150,
                            panel_y + 158 + row * 88,
                            136,
                            76,
                            keys[index],
                            vt_scene_renderer::EditorAction::Character,
                            character);
        }
        draw_editor_key(targets, panel_x + 500, panel_y + 158, 170, 76, "COFNIJ", vt_scene_renderer::EditorAction::Backspace);
        draw_editor_key(targets, panel_x + 688, panel_y + 158, 170, 76, "WYCZYSC", vt_scene_renderer::EditorAction::Clear);
        draw_editor_key(targets, panel_x + 500, panel_y + 422, 170, 76, "ANULUJ", vt_scene_renderer::EditorAction::Cancel);
        draw_editor_key(targets, panel_x + 688, panel_y + 422, 170, 76, "OK", vt_scene_renderer::EditorAction::Confirm, '\0', 0, true);
        return;
    }

    constexpr const char *rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    constexpr int row_lengths[] = { 10, 10, 9, 7 };
    for (int row = 0; row < 4; ++row)
    {
        const int key_width = 76;
        const int gap = 6;
        const int row_width = row_lengths[row] * key_width + (row_lengths[row] - 1) * gap;
        const int start_x = panel_x + (panel_width - row_width) / 2;
        for (int column = 0; column < row_lengths[row]; ++column)
        {
            char label[2] = { rows[row][column], '\0' };
            draw_editor_key(targets,
                            start_x + column * (key_width + gap),
                            panel_y + 150 + row * 67,
                            key_width,
                            58,
                            label,
                            vt_scene_renderer::EditorAction::Character,
                            label[0]);
        }
    }
    draw_editor_key(targets, panel_x + 30, panel_y + 430, 160, 66, "COFNIJ", vt_scene_renderer::EditorAction::Backspace);
    draw_editor_key(targets, panel_x + 202, panel_y + 430, 160, 66, "WYCZYSC", vt_scene_renderer::EditorAction::Clear);
    draw_editor_key(targets, panel_x + 374, panel_y + 430, 180, 66, "SPACJA", vt_scene_renderer::EditorAction::Character, ' ');
    draw_editor_key(targets, panel_x + 566, panel_y + 430, 140, 66, "ANULUJ", vt_scene_renderer::EditorAction::Cancel);
    draw_editor_key(targets, panel_x + 718, panel_y + 430, 140, 66, "OK", vt_scene_renderer::EditorAction::Confirm, '\0', 0, true);
}
#endif
}

namespace vt_scene_renderer
{
bool render(const std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> &working_set,
            const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &working_sets,
            const std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> &auxiliary_working_sets)
{
#if ISOVT_ENABLE_AGISOSTACK
    if (!lcd_panel::is_ready())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(scene_mutex);
    if (terminal_menu_open.load(std::memory_order_relaxed))
    {
        if (lcd_panel::begin_frame() != ESP_OK)
        {
            ESP_LOGW(TAG, "Could not acquire an RGB frame for terminal menu");
            return false;
        }
        vt_view::set_working_set_visible(true);
        std::vector<ScreenRect> next_targets;
        lcd_panel::reset_view_transform();
        lcd_panel::reset_clip_rect();
        render_terminal_menu(next_targets, working_set, working_sets, auxiliary_working_sets);
        render_input_editor(next_targets);
        if (lcd_panel::present() != ESP_OK)
        {
            ESP_LOGW(TAG, "Could not present terminal menu RGB frame");
            return false;
        }
        {
            std::lock_guard<std::mutex> targets_lock(hit_targets_mutex);
            hit_targets = std::move(next_targets);
        }
        scene_visible = true;
        return true;
    }

    if (working_set == nullptr)
    {
        return false;
    }
    const ObjectTree &tree = working_set->get_object_tree();
    const auto data_mask = active_data_mask(tree, working_set);
    if (data_mask == nullptr)
    {
        ESP_LOGW(TAG, "No renderable data mask in working set");
        return false;
    }
    if (lcd_panel::begin_frame() != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not acquire an RGB frame for working set render");
        return false;
    }
    vt_view::set_working_set_visible(true);

    std::vector<ScreenRect> next_targets;
    RenderContext context = {
        .working_set = working_set,
        .object_tree = tree,
        .data_mask_id = data_mask->get_id(),
        .targets = next_targets,
    };

    const auto layout = current_layout();
    lcd_panel::set_view_transform(layout.screen_x,
                                  0,
                                  layout.scale_numerator,
                                  layout.scale_denominator);
    set_logical_clip(layout.data_mask_x, 0, layout.data_mask_size, layout.data_mask_size);
    if (!first_child_covers_data_mask(context, data_mask, layout.data_mask_size))
    {
        lcd_panel::fill_rect(layout.data_mask_x, 0, layout.data_mask_size, layout.data_mask_size,
                             colour(context, data_mask->get_background_color()));
    }
    render_children(context, data_mask, layout.data_mask_x, 0, 0);

    std::uint16_t soft_key_mask_id = NULL_OBJECT_ID;
    if (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
    {
        soft_key_mask_id = std::static_pointer_cast<isobus::DataMask>(data_mask)->get_soft_key_mask();
    }
    else if (data_mask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask)
    {
        soft_key_mask_id = std::static_pointer_cast<isobus::AlarmMask>(data_mask)->get_soft_key_mask();
    }
    const auto soft_key_mask = find_object(tree, soft_key_mask_id);
    set_logical_clip(layout.soft_key_x,
                     0,
                     SOFT_KEY_COLUMNS * layout.soft_key_width,
                     layout.data_mask_size);
    if ((soft_key_mask != nullptr) && (soft_key_mask->get_object_type() == isobus::VirtualTerminalObjectType::SoftKeyMask))
    {
        render_soft_keys(context, soft_key_mask);
    }
    else
    {
        lcd_panel::fill_rect(layout.soft_key_x,
                             0,
                             SOFT_KEY_COLUMNS * layout.soft_key_width,
                             layout.data_mask_size,
                             CHROME_DARK);
    }

    set_logical_clip(VT_ORIGIN_X,
                     0,
                     layout.working_set_bar_width,
                     layout.data_mask_size);
    render_working_set_bar(context, working_sets);
    set_logical_clip(layout.working_set_bar_width,
                     0,
                     layout.working_set_gap,
                     layout.data_mask_size);
    lcd_panel::fill_rect(layout.working_set_bar_width,
                         0,
                         layout.working_set_gap,
                         layout.data_mask_size,
                         CHROME_DARK);
    lcd_panel::reset_view_transform();
    lcd_panel::reset_clip_rect();
    lcd_panel::fill_rect(0, 0, layout.screen_x, board::LCD_HEIGHT, CHROME_DARK);
    const int content_right = screen_x(layout.content_width);
    lcd_panel::fill_rect(content_right,
                         0,
                         board::LCD_WIDTH - content_right,
                         board::LCD_HEIGHT,
                         CHROME_BACKGROUND);
    constexpr int menu_x = 16;
    constexpr int menu_y = 202;
    constexpr int menu_width = 64;
    constexpr int menu_height = 196;
    lcd_panel::fill_rect(menu_x, menu_y, menu_width, menu_height, CHROME_ACTIVE);
    draw_outline(menu_x, menu_y, menu_width, menu_height, CHROME_TEXT, 3);
    lcd_panel::fill_rect(menu_x + 18, menu_y + 18, 28, 4, CHROME_TEXT);
    lcd_panel::fill_rect(menu_x + 18, menu_y + 29, 28, 4, CHROME_TEXT);
    lcd_panel::fill_rect(menu_x + 18, menu_y + 40, 28, 4, CHROME_TEXT);
    lcd_panel::draw_text(menu_x + 26, menu_y + 65, 2, "M", CHROME_TEXT);
    lcd_panel::draw_text(menu_x + 26, menu_y + 93, 2, "E", CHROME_TEXT);
    lcd_panel::draw_text(menu_x + 26, menu_y + 121, 2, "N", CHROME_TEXT);
    lcd_panel::draw_text(menu_x + 26, menu_y + 149, 2, "U", CHROME_TEXT);
    add_screen_target(next_targets, menu_x, menu_y, menu_width, menu_height, TerminalAction::ToggleMenu);

    render_input_editor(next_targets, &context);

    if (lcd_panel::present() != ESP_OK)
    {
        ESP_LOGW(TAG, "Could not present working set RGB frame");
        return false;
    }
    {
        std::lock_guard<std::mutex> targets_lock(hit_targets_mutex);
        hit_targets = std::move(next_targets);
    }
    scene_visible = true;
    return true;
#else
    (void)working_set;
    (void)working_sets;
    return false;
#endif
}

HitTarget hit_test(std::uint16_t x, std::uint16_t y)
{
    std::lock_guard<std::mutex> lock(hit_targets_mutex);
    for (auto target = hit_targets.rbegin(); target != hit_targets.rend(); ++target)
    {
        if ((x >= target->x) && (x < target->x + target->width) &&
            (y >= target->y) && (y < target->y + target->height))
        {
            return target->target;
        }
    }
    return {};
}

bool is_menu_open()
{
    return terminal_menu_open.load(std::memory_order_relaxed);
}

void apply_terminal_action(TerminalAction action)
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    switch (action)
    {
        case TerminalAction::ToggleMenu:
            terminal_menu_open.store(!terminal_menu_open.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
            terminal_notice.clear();
            break;
        case TerminalAction::ShowMachines:
        case TerminalAction::ShowTerminal:
        case TerminalAction::ShowIsobus:
        case TerminalAction::ShowAuxN:
        case TerminalAction::ShowGnss:
        case TerminalAction::ShowAudio:
        case TerminalAction::ShowInformation:
            terminal_page = action;
            terminal_notice.clear();
            break;
        case TerminalAction::AuxFunctionPreviousPage:
            if (aux_function_page > 0U)
            {
                --aux_function_page;
            }
            break;
        case TerminalAction::AuxFunctionNextPage:
            ++aux_function_page;
            break;
        case TerminalAction::AuxInputPreviousPage:
            if (aux_input_page > 0U)
            {
                --aux_input_page;
            }
            break;
        case TerminalAction::AuxInputNextPage:
            ++aux_input_page;
            break;
        case TerminalAction::CycleLanguage:
            terminal_settings.language = static_cast<TerminalLanguage>(
                (static_cast<std::uint8_t>(terminal_settings.language) + 1U) %
                static_cast<std::uint8_t>(TerminalLanguage::Count));
            break;
        case TerminalAction::CycleDateFormat:
            terminal_settings.date_format = static_cast<TerminalDateFormat>(
                (static_cast<std::uint8_t>(terminal_settings.date_format) + 1U) % 3U);
            break;
        case TerminalAction::ToggleTimeFormat:
            terminal_settings.twenty_four_hour = !terminal_settings.twenty_four_hour;
            break;
        case TerminalAction::ToggleDecimalSymbol:
            terminal_settings.decimal_comma = !terminal_settings.decimal_comma;
            break;
        case TerminalAction::ToggleUnits:
            terminal_settings.metric_units = !terminal_settings.metric_units;
            break;
        case TerminalAction::CycleTerminalNumber:
            terminal_settings.terminal_number = static_cast<std::uint8_t>((terminal_settings.terminal_number % 32U) + 1U);
            break;
        case TerminalAction::CycleDataMaskSize:
            terminal_settings.data_mask_size = terminal_settings.data_mask_size == 600 ? 480 : 600;
            break;
        case TerminalAction::DecreaseControlVolume:
            terminal_settings.control_volume = terminal_settings.control_volume >= 5U ?
                                                   terminal_settings.control_volume - 5U : 0U;
            break;
        case TerminalAction::IncreaseControlVolume:
            terminal_settings.control_volume = std::min<std::uint8_t>(100U, terminal_settings.control_volume + 5U);
            break;
        case TerminalAction::DecreaseAlarmVolume:
            terminal_settings.alarm_volume = terminal_settings.alarm_volume >= 5U ?
                                                 terminal_settings.alarm_volume - 5U : 0U;
            break;
        case TerminalAction::IncreaseAlarmVolume:
            terminal_settings.alarm_volume = std::min<std::uint8_t>(100U, terminal_settings.alarm_volume + 5U);
            break;
        case TerminalAction::ToggleAudioMute:
            terminal_settings.audio_muted = !terminal_settings.audio_muted;
            break;
        case TerminalAction::ToggleAuxOLegacy:
            terminal_settings.aux_o_legacy = !terminal_settings.aux_o_legacy;
            aux_n::set_legacy_type1_enabled(terminal_settings.aux_o_legacy);
            break;
        default:
            break;
    }
}

void close_menu()
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    terminal_menu_open.store(false, std::memory_order_relaxed);
    terminal_notice.clear();
}

void set_terminal_settings(const TerminalSettings &settings)
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    terminal_settings = settings;
}

TerminalSettings get_terminal_settings()
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    return terminal_settings;
}

void set_metric_units(bool metric)
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    terminal_settings.metric_units = metric;
}

bool get_metric_units()
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    return terminal_settings.metric_units;
}

void set_notice(const char *text)
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    terminal_notice = text == nullptr ? "" : text;
}

void open_input_editor(InputEditorKind kind,
                       std::uint16_t object_id,
                       const std::string &initial_value,
                       std::size_t maximum_length,
                       const std::vector<EditorListItem> &list_items,
                       std::uint16_t selected_index,
                       EditorTextEncoding text_encoding)
{
    std::lock_guard<std::mutex> lock(input_editor_mutex);
    input_editor.open = true;
    input_editor.kind = kind;
    input_editor.object_id = object_id;
    input_editor.value = initial_value;
    while (!input_editor.value.empty() && (input_editor.value.back() == ' '))
    {
        input_editor.value.pop_back();
    }
    input_editor.maximum_length = std::max<std::size_t>(1, maximum_length);
    input_editor.list_items = list_items;
    input_editor.selected_index = list_items.empty() ? 0 :
                                  std::min<std::uint16_t>(selected_index,
                                                          static_cast<std::uint16_t>(list_items.size() - 1));
    input_editor.text_encoding = text_encoding;
}

InputEditorResult apply_editor_action(EditorAction action, char character, std::uint16_t index)
{
    std::lock_guard<std::mutex> lock(input_editor_mutex);
    InputEditorResult result;
    if (!input_editor.open)
    {
        return result;
    }

    result.handled = true;
    result.kind = input_editor.kind;
    result.object_id = input_editor.object_id;
    result.text_encoding = input_editor.text_encoding;
    switch (action)
    {
        case EditorAction::Character:
            if ((input_editor.value.size() < input_editor.maximum_length) &&
                ((input_editor.kind != InputEditorKind::Number) ||
                 ((character >= '0' && character <= '9') || (character == '-') || (character == '.') || (character == ','))))
            {
                if (((character == '.') || (character == ',')) &&
                    ((input_editor.value.find('.') != std::string::npos) ||
                     (input_editor.value.find(',') != std::string::npos)))
                {
                    break;
                }
                if ((character == '-') && !input_editor.value.empty())
                {
                    break;
                }
                input_editor.value.push_back(character);
            }
            break;
        case EditorAction::Backspace:
            if (!input_editor.value.empty())
            {
                input_editor.value.pop_back();
            }
            break;
        case EditorAction::Clear:
            input_editor.value.clear();
            break;
        case EditorAction::Previous:
            if (input_editor.selected_index > 0)
            {
                --input_editor.selected_index;
            }
            break;
        case EditorAction::Next:
            if ((input_editor.selected_index + 1U) < input_editor.list_items.size())
            {
                ++input_editor.selected_index;
            }
            break;
        case EditorAction::Select:
            if (index < input_editor.list_items.size())
            {
                input_editor.selected_index = index;
            }
            break;
        case EditorAction::Cancel:
            result.cancelled = true;
            input_editor.open = false;
            break;
        case EditorAction::Confirm:
            result.committed = true;
            result.value = input_editor.value;
            result.selected_index = input_editor.selected_index;
            input_editor.open = false;
            break;
        case EditorAction::None:
            break;
    }
    result.value = input_editor.value;
    result.selected_index = input_editor.selected_index;
    return result;
}

bool is_input_editor_open()
{
    std::lock_guard<std::mutex> lock(input_editor_mutex);
    return input_editor.open;
}

void close_input_editor()
{
    std::lock_guard<std::mutex> lock(input_editor_mutex);
    input_editor = {};
}

void clear()
{
    std::lock_guard<std::mutex> lock(scene_mutex);
    {
        std::lock_guard<std::mutex> targets_lock(hit_targets_mutex);
        hit_targets.clear();
    }
    scene_visible = false;
    vt_view::set_working_set_visible(false);
}
}
