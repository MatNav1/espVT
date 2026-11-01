#pragma once

#include <cstdint>

// Roboto Mono rasterized at runtime for any character cell size.
namespace vt_font
{
struct Glyph
{
    std::uint16_t width;
    std::uint16_t height;
    const std::uint8_t *alpha; // width * height coverage values, 0..255
};

// Returns the glyph filling a width x height cell, or nullptr when the font has
// no glyph for the codepoint. The result stays valid until the next call.
const Glyph *get(std::uint16_t codepoint, int width, int height);
}
