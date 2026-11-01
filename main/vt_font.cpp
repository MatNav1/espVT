#include "vt_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "stb_truetype.h"

extern "C"
{
extern const std::uint8_t roboto_mono_start[] asm("_binary_RobotoMono_Regular_ttf_start");
}

namespace
{
constexpr char TAG[] = "vt_font";
// The cell spans this much of the em square above and below the baseline, enough
// for accented capitals (A with diaeresis, Cyrillic short I) and descenders.
constexpr float CELL_ASCENT_EM = 0.92F;
constexpr float CELL_DESCENT_EM = 0.23F;
// Rendered glyphs are kept until the cache exceeds this size, then it starts over.
constexpr std::size_t CACHE_BUDGET_BYTES = 2 * 1024 * 1024;
constexpr int MAXIMUM_CELL_SIZE = 1024;

struct CachedGlyph
{
    vt_font::Glyph glyph;
    std::uint8_t alpha[1];
};

std::mutex cache_mutex;
std::unordered_map<std::uint64_t, CachedGlyph *> cache;
std::size_t cache_bytes = 0;
stbtt_fontinfo font;
bool font_ready = false;
bool font_failed = false;

bool init_font()
{
    if (font_ready || font_failed)
    {
        return font_ready;
    }
    const int offset = stbtt_GetFontOffsetForIndex(roboto_mono_start, 0);
    font_ready = (offset >= 0) && (stbtt_InitFont(&font, roboto_mono_start, offset) != 0);
    font_failed = !font_ready;
    if (!font_ready)
    {
        ESP_LOGE(TAG, "Embedded Roboto Mono could not be parsed");
    }
    return font_ready;
}

void clear_cache()
{
    for (auto &entry : cache)
    {
        heap_caps_free(entry.second);
    }
    cache.clear();
    cache_bytes = 0;
}

// The cell height covers CELL_ASCENT_EM + CELL_DESCENT_EM of the em square, so
// nothing is clipped; the advance is stretched to the cell width, so square
// ISO 11783 font sizes get wide characters instead of narrow centered ones.
CachedGlyph *render(int glyph_index, int width, int height)
{
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    auto *entry = static_cast<CachedGlyph *>(
        heap_caps_calloc_prefer(1, sizeof(CachedGlyph) + pixels, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
    if (entry == nullptr)
    {
        return nullptr;
    }
    entry->glyph.width = static_cast<std::uint16_t>(width);
    entry->glyph.height = static_cast<std::uint16_t>(height);
    entry->glyph.alpha = entry->alpha;

    int advance = 0;
    int left_bearing = 0;
    stbtt_GetGlyphHMetrics(&font, glyph_index, &advance, &left_bearing);
    const float em = height / (CELL_ASCENT_EM + CELL_DESCENT_EM);
    const float scale_y = stbtt_ScaleForMappingEmToPixels(&font, em);
    const float scale_x = (advance > 0) ? static_cast<float>(width) / advance : scale_y;
    const float baseline = CELL_ASCENT_EM * em;
    const float baseline_floor = std::floor(baseline);

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&font, glyph_index, scale_x, scale_y, 0.0F, baseline - baseline_floor,
                                    &x0, &y0, &x1, &y1);
    const int box_width = x1 - x0;
    const int box_height = y1 - y0;
    if ((box_width <= 0) || (box_height <= 0))
    {
        return entry; // blank glyph such as the space
    }

    auto *box = static_cast<std::uint8_t *>(
        heap_caps_malloc_prefer(static_cast<std::size_t>(box_width) * box_height, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT));
    if (box == nullptr)
    {
        heap_caps_free(entry);
        return nullptr;
    }
    stbtt_MakeGlyphBitmapSubpixel(&font, box, box_width, box_height, box_width, scale_x, scale_y,
                                  0.0F, baseline - baseline_floor, glyph_index);

    const int top = static_cast<int>(baseline_floor) + y0;
    for (int row = std::max(0, -top); row < box_height; ++row)
    {
        const int cell_y = top + row;
        if (cell_y >= height)
        {
            break;
        }
        const int first = std::max(0, -x0);
        const int last = std::min(box_width, width - x0);
        if (first < last)
        {
            std::memcpy(entry->alpha + static_cast<std::size_t>(cell_y) * width + x0 + first,
                        box + static_cast<std::size_t>(row) * box_width + first,
                        static_cast<std::size_t>(last - first));
        }
    }
    heap_caps_free(box);
    return entry;
}
}

namespace vt_font
{
const Glyph *get(std::uint16_t codepoint, int width, int height)
{
    if ((width <= 0) || (height <= 0) || (width > MAXIMUM_CELL_SIZE) || (height > MAXIMUM_CELL_SIZE))
    {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(cache_mutex);
    if (!init_font())
    {
        return nullptr;
    }
    const int glyph_index = stbtt_FindGlyphIndex(&font, codepoint);
    if ((glyph_index == 0) && (codepoint != ' '))
    {
        return nullptr;
    }

    const std::uint64_t key = (static_cast<std::uint64_t>(codepoint) << 32U) |
                              (static_cast<std::uint64_t>(width) << 16U) |
                              static_cast<std::uint64_t>(height);
    const auto found = cache.find(key);
    if (found != cache.end())
    {
        return &found->second->glyph;
    }

    const std::size_t bytes = sizeof(CachedGlyph) + static_cast<std::size_t>(width) * height;
    if (cache_bytes + bytes > CACHE_BUDGET_BYTES)
    {
        clear_cache();
    }
    CachedGlyph *entry = render(glyph_index, width, height);
    if (entry == nullptr)
    {
        return nullptr;
    }
    cache.emplace(key, entry);
    cache_bytes += bytes;
    return &entry->glyph;
}
}
