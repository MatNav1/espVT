#pragma once

#include "esp_err.h"

// Touch controller of the selected board. Touches are delivered in LCD
// coordinates to isovt::on_touch_pressed()/on_touch_released().
namespace touch
{
esp_err_t start();
bool is_ready();
}
