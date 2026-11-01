#include "ui_text.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace
{
constexpr std::size_t LANGUAGE_COUNT = static_cast<std::size_t>(ui_text::Language::Count);
using Row = std::array<const char *, LANGUAGE_COUNT>;

constexpr Row TEXTS[] = {
#define UI_TEXT_ROW(id, english, german, polish) { english, german, polish },
    UI_TEXTS(UI_TEXT_ROW)
#undef UI_TEXT_ROW
};
static_assert(sizeof(TEXTS) / sizeof(TEXTS[0]) == static_cast<std::size_t>(ui_text::Id::Count));

std::atomic<ui_text::Language> current_language{ ui_text::Language::Polish };
}

namespace ui_text
{
void set_language(Language language)
{
    if (language < Language::Count)
    {
        current_language.store(language, std::memory_order_relaxed);
    }
}

Language language()
{
    return current_language.load(std::memory_order_relaxed);
}

const char *get(Id id)
{
    if (id >= Id::Count)
    {
        return "";
    }
    const Row &row = TEXTS[static_cast<std::size_t>(id)];
    const char *translated = row[static_cast<std::size_t>(language())];
    return translated != nullptr ? translated : row[static_cast<std::size_t>(Language::English)];
}

std::string format(Id id, ...)
{
    char buffer[160];
    va_list arguments;
    va_start(arguments, id);
    std::vsnprintf(buffer, sizeof(buffer), get(id), arguments);
    va_end(arguments);
    return buffer;
}

Text text(Id id)
{
    return [id]() { return std::string(get(id)); };
}

Text text(Id id, unsigned value)
{
    return [id, value]() { return format(id, value); };
}
}
