#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace touch_gt911
{
struct Point
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t strength;
    std::uint8_t id;
};

esp_err_t start();
bool is_ready();
}
